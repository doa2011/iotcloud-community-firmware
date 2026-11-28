/* Start the shared UART owner after validating the explicit installation path.
 * Stock UART auto-installs complete applications; it cannot stage-only. */
static const char *screenUpdateStart(JsonObjectConst params) {
  String mode = params["mode"] | "";
  bool bootstrap = mode == "factory-bootstrap";
  if ((mode != "stage" && !bootstrap) ||
      !screenFields(params, "|mode||sha256|") ||
      params.size() != (bootstrap ? 2u : 1u))
    return "invalid_params";
  if (screenJpegBusy.load() || screenUploadBusy || screenWallpaper.active || screenWallpaper.pending || screenFontBusy())
    return "busy";
  screenImportArtifact();
  if (!screenImage.valid)
    return "not_ready";
  if (screenOtaPrepare.phase || screenImage.active || screenInstallPending())
    return "busy";
  if (bootstrap) {
    if (screenGuardAvailable() ||
        !params["sha256"].is<const char *>() ||
        String(params["sha256"].as<const char *>()) != screenHex(screenImage.sha, 32))
      return "invalid_firmware";
  } else if (!screenGuardAvailable())
    return "not_ready";
  if (!screenStoreMutex || xSemaphoreTake(screenStoreMutex, 0) != pdTRUE)
    return "busy";
  if (!screenImageVerify()) {
    xSemaphoreGive(screenStoreMutex);
    return "invalid_firmware";
  }
  const bool prepare = !bootstrap && screenGuard.mode == 0;
  portENTER_CRITICAL(&screenImageMux);
  screenImage.active = !prepare;
  if (prepare) screenOtaPrepare.phase = 1;
  // Delayed business replies must not interleave with factory raw blocks.
  while (screenHead != screenTail) {
    screenQueue[screenHead].wire = "";
    screenHead = (screenHead + 1) % 12;
  }
  portEXIT_CRITICAL(&screenImageMux);
  xSemaphoreGive(screenStoreMutex);
  screenImage.pending = false;
  screenImage.bootstrap = bootstrap;
  screenImage.installSession = screenGuard.session;
  screenImage.offset = 0;
  screenOtaError = "";
  screenImage.deadline = millis() + 60000;
  screenImage.state = "waiting_for_mcu";
  if (!screenMediaQuiesce()) {
    screenOtaPrepare = {};
    screenImage.active = false;
    screenImage.state = "uart_busy";
    return "busy";
  }
  if (prepare) {
    StaticJsonDocument<128> receipt;
    screenOtaPrepare.session = screenGuard.session;
    const char *code = screenGuardRequest(GUARD_RESET, 0, 0, receipt.to<JsonObject>());
    if (strcmp(code, "accepted")) {
      screenOtaPrepare = {};
      screenImage.state = "install_failed";
      screenOtaError = "maintenance_reset_rejected";
      invalidateBusinessStatusCache();
      return code;
    }
    screenOtaPrepare.request = screenGuard.request;
    screenOtaPrepare.deadline = millis() + 60000;
    invalidateBusinessStatusCache();
    return "accepted";
  }
  bool sent;
  if (bootstrap) {
    DynamicJsonDocument request(512);
    request["client_token"] = "iotcloud-bootstrap-" + String(millis());
    JsonObject data = request.createNestedObject("data");
    data["upgrade_method"] = 1;
    JsonObject target = data.createNestedArray("upgrade_info").createNestedObject();
    target["device_type"] = "SENSOR3_MCU";
    target["to_version"] = "SV.99.00.01.00";
    sent = screenDelivery(screenRoot() + "/mqtt/service/ota", request, 0);
  } else
    sent = screenSend("+screen_recover:1\r\n");
  if (!sent) {
    screenImage.active = false;
    return "busy";
  }
  return "accepted";
}

// Follow the same reset -> new forced recovery session -> Quad ACK -> stage
// sequence that passed on the physical BRY88BBA15G. Never fall back to staging
// from the normal application if any preparation step fails.
static void screenOtaPrepareFail(const char *error) {
  screenOtaPrepare = {};
  screenImage.active = screenImage.pending = false;
  screenImage.state = "install_failed";
  screenOtaError = error;
  invalidateBusinessStatusCache();
}
static void screenOtaPreparePoll(uint32_t now) {
  if (!screenOtaPrepare.phase) return;
  if ((int32_t)(now - screenOtaPrepare.deadline) >= 0) {
    screenOtaPrepareFail("maintenance_prepare_timeout");
    return;
  }
  if (!screenGuardAvailable()) return;
  if (screenOtaPrepare.phase == 1) {
    if (screenGuard.session == screenOtaPrepare.session) {
      if (screenGuard.request == screenOtaPrepare.request &&
          !strcmp(screenGuard.state, "mcu_rejected"))
        screenOtaPrepareFail("maintenance_reset_rejected");
      return;
    }
    if (screenGuard.mode != 3 || !screenGuard.activation) {
      screenOtaPrepareFail("maintenance_not_held");
      return;
    }
    StaticJsonDocument<128> receipt;
    const char *code = screenGuardRequest(GUARD_NOR_MODE, GUARD_NOR_SFC_QUAD, 0,
                                          receipt.to<JsonObject>());
    if (strcmp(code, "accepted")) {
      screenOtaPrepareFail("maintenance_nor_request_failed");
      return;
    }
    screenOtaPrepare.phase = 2;
    screenOtaPrepare.session = screenGuard.session;
    screenOtaPrepare.request = screenGuard.request;
    screenOtaPrepare.deadline = millis() + 15000;
    invalidateBusinessStatusCache();
    return;
  }
  if (screenGuard.session != screenOtaPrepare.session || screenGuard.mode != 3 ||
      screenGuard.request != screenOtaPrepare.request ||
      screenGuard.operation != GUARD_NOR_MODE) {
    screenOtaPrepareFail("maintenance_session_changed");
    return;
  }
  if (!strcmp(screenGuard.state, "pending")) return;
  if (strcmp(screenGuard.state, "complete") || screenGuard.result != GUARD_OK) {
    screenOtaPrepareFail("maintenance_nor_rejected");
    return;
  }
  // The image was already hashed before entering recovery; uploads are blocked
  // throughout preparation. Set the raw owner before releasing the prep lease.
  screenImage.active = true;
  screenImage.installSession = screenGuard.session;
  screenImage.deadline = millis() + 60000;
  screenOtaPrepare = {};
  if (!screenSend("+screen_recover:1\r\n")) {
    screenOtaPrepareFail("maintenance_stage_send_failed");
    return;
  }
  invalidateBusinessStatusCache();
}
