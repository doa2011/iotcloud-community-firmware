static esp_err_t screenUploadHandler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_OK;
  char target[48] = {};
  if (httpd_req_get_hdr_value_str(req, "X-IotCloud-Target", target,
                                  sizeof(target)) != ESP_OK ||
      strcmp(target, "BRY88BBA15G-screen") ||
      req->content_len != BRY_IMAGE_LIMIT)
    return jsonResponse(req, "400 Bad Request",
                        "{\"code\":\"invalid_firmware\"}");
  ScreenMediaOtaScope mediaScope;
  if(!mediaScope.begin(req))
    return jsonResponse(req,"503 Service Unavailable","{\"code\":\"media_handoff_failed\"}");
  if (!screenStoreAvailable || !screenStoreMutex ||
      xSemaphoreTake(screenStoreMutex, 0) != pdTRUE)
    return jsonResponse(req, "503 Service Unavailable", "{\"code\":\"busy\"}");
  bool busy;
  portENTER_CRITICAL(&screenImageMux);
  busy = g3OnlineBusy || otaActive || screenOtaPowerLeaseActive() || !strcmp(screenImage.state, "staged_verified") || screenUploadBusy || screenWallpaper.active || screenWallpaper.pending;
  if (!busy)
    screenUploadBusy = true;
  portEXIT_CRITICAL(&screenImageMux);
  if (busy) {
    xSemaphoreGive(screenStoreMutex);
    return jsonResponse(req, "503 Service Unavailable", "{\"code\":\"busy\"}");
  }
  bool powerReady = screenModulePowerBegin();
  if (!powerReady) {
    portENTER_CRITICAL(&screenImageMux);
    screenUploadBusy = false;
    portEXIT_CRITICAL(&screenImageMux);
    xSemaphoreGive(screenStoreMutex);
    return jsonResponse(req, "503 Service Unavailable", "{\"code\":\"busy\"}");
  }
  bool ok = bry_store_claim(&screenStore) &&
            screenEraseStore(nullptr, BRY_IMAGE_HEADER, 4096);
  ScreenUploadErase eraser(BRY_IMAGE_START, BRY_IMAGE_LIMIT,
                           screenStorePartition.size);
  mbedtls_md5_context md5;
  mbedtls_sha256_context sha;
  mbedtls_md5_init(&md5);
  mbedtls_sha256_init(&sha);
  mbedtls_md5_starts_ret(&md5);
  mbedtls_sha256_starts_ret(&sha, 0);
  uint8_t bytes[1024], h[BRY_IMAGE_HEADER_BYTES] = {}, vectors[8] = {};
  uint32_t at = 0;
  bool identity = false, vector = false, recovery = false, activation = false;
  String tail;
  const char *signature =
      "iotcloud-screen/BRY88BBA15G;application=0x14000;";
  while (ok && at < BRY_IMAGE_LIMIT) {
    if (!screenModulePowerHealthy()) { ok = false; break; }
    int n = httpd_req_recv(req, (char *)bytes,
                           min((uint32_t)sizeof(bytes), BRY_IMAGE_LIMIT - at));
    if (n <= 0) {
      ok = false;
      break;
    }
    if (at < 8)
      memcpy(vectors + at, bytes, min((unsigned)n, 8u - at));
    if (at + (unsigned)n >= 8) {
      uint32_t sp = scene_u32(vectors), reset = scene_u32(vectors + 4);
      vector = sp == 0x20010000u && (reset & 1) && reset >= 0x14001 &&
               reset < 0x7b000;
    }
    String probe = tail;
    for (int i = 0; i < n; i++)
      probe += (char)(bytes[i] ? bytes[i] : ' ');
    identity = identity || probe.indexOf(signature) >= 0;
    recovery = recovery || probe.indexOf("install=factory-uart;mode=recovery;") >= 0;
    activation = activation || probe.indexOf("install=factory-uart;") >= 0;
    tail = probe.substring(probe.length() > 128 ? probe.length() - 128 : 0);
    ok = eraser.prepare(at, n, [](uint32_t address, size_t size) {
           return screenEraseStore(nullptr, address, size);
         }) && screenWriteStore(nullptr, BRY_IMAGE_START + at, bytes, n) &&
         !mbedtls_md5_update_ret(&md5, bytes, n) &&
         !mbedtls_sha256_update_ret(&sha, bytes, n);
    at += n;
    delay(1);
  }
  uint8_t m[16], s[32], verifyM[16], verifyS[32];
  ok = ok && vector && identity && at == BRY_IMAGE_LIMIT &&
       !mbedtls_md5_finish_ret(&md5, m) && !mbedtls_sha256_finish_ret(&sha, s);
  mbedtls_md5_free(&md5);
  mbedtls_sha256_free(&sha);
  char auth[80] = {};
  ph_auth_context context;
  requestAuthorization(req, auth);
  bool stillAuthorized = localAuthReady && !configRecovery &&
                         ph_auth_http_check(&localAuth, auth, &context);
  ok = ok && stillAuthorized && screenHash(verifyM, verifyS) &&
       !memcmp(m, verifyM, 16) && !memcmp(s, verifyS, 32);
  if (ok) {
    bry_image_header_encode(h, BRY_IMAGE_LIMIT, m, s,
                            (recovery ? 1u : 0u) | (activation ? 2u : 0u));
    uint8_t check[BRY_IMAGE_HEADER_BYTES];
    ok = screenWriteStore(nullptr, BRY_IMAGE_HEADER, h, sizeof(h)) &&
         screenReadStore(nullptr, BRY_IMAGE_HEADER, check, sizeof(check)) &&
         !memcmp(h, check, sizeof(h));
  }
  if (!ok)
    memset(h, 0, sizeof(h));
  portENTER_CRITICAL(&screenImageMux);
  memcpy(screenArtifactHeader, h, sizeof(h));
  screenArtifactPending = true;
  screenUploadBusy = false;
  portEXIT_CRITICAL(&screenImageMux);
  xSemaphoreGive(screenStoreMutex);
  if (powerReady) screenModulePowerEnd();
  if (!stillAuthorized)
    return jsonResponse(req, "401 Unauthorized", "{\"code\":\"unauthorized\"}");
  if (!ok)
    return jsonResponse(req, "400 Bad Request",
                        "{\"code\":\"invalid_firmware\"}");
  DynamicJsonDocument response(768);
  response["code"] = "ok";
  response["data"]["sha256"] = screenHex(s, 32);
  response["data"]["md5"] = screenHex(m, 16);
  response["data"]["size"] = at;
  response["data"]["installable"] = false;
  String wire;
  serializeWireMP(response, wire);
  return jsonResponse(req, "200 OK", wire);
}


// HTTP worker owns the input mailbox only while webDone=true. The loop owns
// all UART writes and ACK checks. No image-sized RAM allocation or Flash staging.
#include "screen_jpeg_upload.h"
static struct {
  ScreenJpegHeader header;
  uint8_t packet[MEDIA_CHUNK+9]={};
  size_t length=0;
  uint32_t sequence=0,session=0,deadline=0;
  const char *result="idle";
  bool album=false;
} screenJpeg;
// Retain only a small transfer descriptor between HTTP batches.
static struct {
  ScreenJpegHeader header;
  uint32_t offset=0,session=0,epoch=0;
  bool active=false;
} screenAlbumBatch;

static const uint8_t *screenJpegSlot(uint32_t id) {
  for(unsigned i=0;i<MEDIA_IMAGE_SLOTS;++i) {
    const uint8_t *p=screenMedia.snapshot+MEDIA_STATUS_IMAGES+17*i;
    if(media_u32(p)==id)return p;
  }
  return nullptr;
}
static void screenJpegFinish(const char *result) {
  screenJpeg.result=result;screenJpeg.sequence=0;
  portENTER_CRITICAL(&webMux);webPending=false;webDone=true;portEXIT_CRITICAL(&webMux);
}
static void screenJpegOwnerPoll() {
  if(!screenJpegBusy.load())return screenJpegFinish("aborted");
  if(!webAuthRequired||!localAuthContextValid(&webAuthContext))
    return screenJpegFinish("unauthorized");
  if(otaActive||screenMediaOtaBlocked()||!screenMediaAvailable())
    return screenJpegFinish("not_ready");
  const uint8_t op=screenJpeg.packet[0];
  const auto &h=screenJpeg.header;
  if(!screenJpeg.sequence) {
    if(screenMedia.pending)return screenJpegFinish("busy");
    if(op==MEDIA_BEGIN) {
      if(screenJpegSlot(h.id))return screenJpegFinish("duplicate_id");
      if(screenJpeg.album&&!screenMediaAlbumOwn(h.id))return screenJpegFinish("album_slots_full");
      screenJpeg.session=screenMedia.session;
    } else if(screenJpeg.session!=screenMedia.session) {
      return screenJpegFinish("session_changed");
    }
    StaticJsonDocument<128> receipt;
    const char *result=screenMediaSubmit(screenJpeg.packet,screenJpeg.length,
                                          receipt.to<JsonObject>(),true);
    if(strcmp(result,"accepted"))return screenJpegFinish(result);
    screenJpeg.sequence=screenMedia.request;
    screenJpeg.deadline=millis()+6500;
    return;
  }
  if(screenJpeg.session!=screenMedia.session||
     screenMedia.request!=screenJpeg.sequence||screenMedia.lastOp!=op||
     screenMedia.lastId!=h.id)
    return screenJpegFinish("superseded");
  if(screenMedia.pending) {
    if((int32_t)(millis()-screenJpeg.deadline)>=0)screenJpegFinish("timeout");
    return;
  }
  if(strcmp(screenMedia.requestState,"complete")||screenMedia.result!=MEDIA_OK)
    return screenJpegFinish("mcu_rejected");
  const uint8_t *slot=screenJpegSlot(h.id);
  if(op==MEDIA_CANCEL) {
    if(slot)return screenJpegFinish("cancel_unconfirmed");
  } else {
    uint32_t offset=op==MEDIA_BEGIN?0:op==MEDIA_COMMIT?h.total:
        media_u32(screenJpeg.packet+5)+screenJpeg.length-9;
    if(!slot||!h.matches(slot,offset,op==MEDIA_COMMIT?2:1))
      return screenJpegFinish("integrity_mismatch");
  }
  screenJpegFinish("ok");
}
static const char *screenJpegExchange(size_t length) {
  if(webPending||!webDone)return "busy";
  screenJpeg.length=length;
  webOperation=5;webDone=false;
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  uint32_t start=millis();
  while(!webDone) {
    if(uint32_t(millis()-start)>=8000)return "owner_timeout";
    delay(1);
  }
  return screenJpeg.result;
}
static esp_err_t screenJpegHandler(httpd_req_t *req) {
  if(!authorized(req))return ESP_OK;
  char type[48]={};
  if(httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK||
     strcmp(type,"application/octet-stream"))
    return jsonResponse(req,"415 Unsupported Media Type","{\"code\":\"invalid_params\"}");
  if(req->content_len<=16||req->content_len>16u+MEDIA_JPEG_MAX)
    return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
  if(webPending||!webDone||otaActive||screenMediaOtaPreparing.load()||screenJpegBusy.exchange(true))
    return jsonResponse(req,"503 Service Unavailable","{\"code\":\"busy\"}");
  struct Release { ~Release(){screenJpegBusy.store(false);} } release;
  const bool batch=!strcmp(req->uri,"/api/v1/screen/media/jpeg/batch");
  uint8_t header[24];
  size_t headerSize=batch?24:16;
  if(!receiveExact(req,header,headerSize))return ESP_FAIL;
  uint32_t offset=0,limit=0;
  ScreenAlbumBatch part;
  screenJpeg.album=batch;
  if(batch) {
    if(!part.parse(header,req->content_len))
      return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
    // The owner also checks generation and session before the first command.
    if(part.epoch!=screenMedia.albumEpoch)
      return jsonResponse(req,"409 Conflict","{\"code\":\"album_preempted\"}");
    screenJpeg.header=part.image;offset=part.offset;
    limit=offset+req->content_len-headerSize;
    if(offset) {
      const auto &saved=screenAlbumBatch.header;
      if(!screenAlbumBatch.active||screenAlbumBatch.epoch!=part.epoch||
         screenAlbumBatch.offset!=offset||saved.id!=part.image.id||
         saved.total!=part.image.total||saved.crc!=part.image.crc||
         saved.width!=part.image.width||saved.height!=part.image.height)
        return jsonResponse(req,"409 Conflict","{\"code\":\"album_preempted\"}");
      screenJpeg.session=screenAlbumBatch.session;
    } else if(screenAlbumBatch.active) {
      // An abandoned upload must be explicitly cancelled or preempted.
      return jsonResponse(req,"409 Conflict","{\"code\":\"album_upload_pending\"}");
    }
  } else {
    if(!screenJpeg.header.parse(header,req->content_len))
      return jsonResponse(req,"400 Bad Request","{\"code\":\"invalid_params\"}");
    limit=screenJpeg.header.total;
  }
  if(!captureWebAuthorization(req))return ESP_OK;
  uint32_t started=millis();
  auto &h=screenJpeg.header;
  const char *result="ok";
  bool began=offset!=0;
  if(!offset) {
    h.begin(screenJpeg.packet);
    result=screenJpegExchange(17);
    began=!strcmp(result,"ok");
  }
  while(!strcmp(result,"ok")&&offset<limit) {
    if(uint32_t(millis()-started)>=110000){result="upload_timeout";break;}
    size_t n=h.chunkSize(offset);
    h.chunk(screenJpeg.packet,offset);
    if(!receiveExact(req,screenJpeg.packet+9,n)){result="upload_incomplete";break;}
    result=screenJpegExchange(n+9);
    if(!strcmp(result,"ok"))offset+=n;
  }
  if(!strcmp(result,"ok")&&offset==h.total) {
    h.terminal(screenJpeg.packet,false);
    result=screenJpegExchange(5);
  }
  if(strcmp(result,"ok")) {
    // Only cancel our confirmed, unreferenced staging image. Never cancel
    // an existing ID, a new session, or a mailbox still in flight.
    if(began&&webDone&&!webPending) {
      h.terminal(screenJpeg.packet,true);
      (void)screenJpegExchange(5);
    }
    const String body=String("{\"code\":\"upload_failed\",\"message\":\"")+result+"\"}";
    // An incomplete body must not be reused as another HTTP request.
    if(batch)screenAlbumBatch.active=false;
    if(offset<limit) {
      jsonResponse(req,"503 Service Unavailable",body);
      return ESP_FAIL;
    }
    return jsonResponse(req,"503 Service Unavailable",body);
  }
  if(batch) {
    screenAlbumBatch.header=h;screenAlbumBatch.offset=offset;
    screenAlbumBatch.session=screenJpeg.session;screenAlbumBatch.epoch=part.epoch;
    screenAlbumBatch.active=offset<h.total;
  }
  StaticJsonDocument<256> reply;
  reply["code"]="ok";JsonObject data=reply.createNestedObject("data");
  data["id"]=h.id;data["total"]=h.total;data["offset"]=offset;
  data["crc32"]=h.crc;data["state"]=offset==h.total?"ready":"receiving";data["elapsed_ms"]=millis()-started;
  if(batch)data["album_epoch"]=part.epoch;
  String wire;serializeWireMP(reply,wire);
  return jsonResponse(req,"200 OK",wire);
}

// HTTP task waits; only loop/owner consumes media ACKs. The gate is held
// through the actual OTA entry point; no new media or baud change during it.
static uint32_t screenMediaOtaSeq=0,screenMediaOtaSession=0;
static uint32_t screenMediaOtaDeadline=0;
static void screenMediaOtaFinish(const char *result) {
  screenMediaOtaResult=result;screenMediaOtaSeq=0;
  portENTER_CRITICAL(&webMux);webPending=false;webDone=true;portEXIT_CRITICAL(&webMux);
}
static void screenMediaOtaPreparePoll() {
  if(!screenMediaOtaPreparing.load()||!webAuthRequired||
     !localAuthContextValid(&webAuthContext))return screenMediaOtaFinish("unauthorized");
  if((int32_t)(millis()-screenMediaOtaDeadline)>=0)return screenMediaOtaFinish("timeout");
  if(screenMedia.pending||screenMedia.phase)return;
  if(screenMediaOtaSession!=screenMedia.session)return screenMediaOtaFinish("session_changed");
  if(screenMediaOtaSeq) {
    if(screenMedia.request!=screenMediaOtaSeq||strcmp(screenMedia.requestState,"complete")||
       screenMedia.result!=MEDIA_OK)return screenMediaOtaFinish("cleanup_unconfirmed");
    screenMediaOtaSeq=0;
  }
  // An uncertain SET cannot be treated as a confirmed empty album.
  if(screenMedia.lastOp==MEDIA_ALBUM_SET&&!strcmp(screenMedia.requestState,"uncertain"))
    return screenMediaOtaFinish("album_state_uncertain");
  uint8_t p[5]={};
  if(screenMedia.albumId) {
    p[0]=MEDIA_ALBUM_CLEAR;media_put32(p+1,screenMedia.albumId);
  } else {
    for(auto &id:screenMedia.albumOwned)if(id) {
      if(!screenJpegSlot(id)){id=0;continue;}
      p[0]=MEDIA_CANCEL;media_put32(p+1,id);break;
    }
  }
  if(p[0]) {
    StaticJsonDocument<128> receipt;
    const char *result=screenMediaSubmit(p,sizeof(p),receipt.to<JsonObject>(),true);
    if(strcmp(result,"accepted"))return screenMediaOtaFinish(result);
    screenMediaOtaSeq=screenMedia.request;
    return;
  }
  screenAlbumBatch.active=false;
  screenRelayStop();
  // Don't change baud on one side alone. Existing raw OTA uses the current
  // confirmed rate; Guard restart handles the coordinated boot-rate return.
  if(uart_wait_tx_done(UART_NUM_1,pdMS_TO_TICKS(200))!=ESP_OK)
    return screenMediaOtaFinish("uart_busy");
  screenMediaOtaFinish("ok");
}
static bool screenMediaOtaBegin(httpd_req_t *req) {
  if(webPending||!webDone||otaActive||screenImage.active||screenJpegBusy.load()||
     screenMediaOtaPreparing.exchange(true))return false;
  if(!captureWebAuthorization(req)){screenMediaOtaPreparing.store(false);return false;}
  screenMediaOtaSeq=0;screenMediaOtaSession=screenMedia.session;
  screenMediaOtaDeadline=millis()+18000;
  // Generation invalidates even a client sleeping between images.
  if(!++screenMedia.albumEpoch)++screenMedia.albumEpoch;
  webOperation=6;webDone=false;
  portENTER_CRITICAL(&webMux);webPending=true;portEXIT_CRITICAL(&webMux);
  const uint32_t start=millis();
  while(!webDone&&uint32_t(millis()-start)<20000)delay(1);
  if(!webDone||strcmp(screenMediaOtaResult,"ok")) {
    screenMediaOtaPreparing.store(false);return false;
  }
  return true;
}
