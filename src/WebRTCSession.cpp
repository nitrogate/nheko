#include <QQmlEngine>
#include <QQuickItem>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#include "ChatPage.h"
#include "Logging.h"
#include "UserSettingsPage.h"
#include "WebRTCSession.h"

#ifdef GSTREAMER_AVAILABLE
extern "C"
{
#include "gst/gst.h"
#include "gst/sdp/sdp.h"

#define GST_USE_UNSTABLE_API
#include "gst/webrtc/webrtc.h"
}
#endif

// https://github.com/vector-im/riot-web/issues/10173
#define STUN_SERVER "stun://turn.matrix.org:3478"

Q_DECLARE_METATYPE(webrtc::State)

using webrtc::State;

WebRTCSession::WebRTCSession()
  : QObject()
  , devices_(CallDevices::instance())
{
        qRegisterMetaType<webrtc::State>();
        qmlRegisterUncreatableMetaObject(
          webrtc::staticMetaObject, "im.nheko", 1, 0, "WebRTCState", "Can't instantiate enum");

        connect(this, &WebRTCSession::stateChanged, this, &WebRTCSession::setState);
        init();
}

bool
WebRTCSession::init(std::string *errorMessage)
{
#ifdef GSTREAMER_AVAILABLE
        if (initialised_)
                return true;

        GError *error = nullptr;
        if (!gst_init_check(nullptr, nullptr, &error)) {
                std::string strError("WebRTC: failed to initialise GStreamer: ");
                if (error) {
                        strError += error->message;
                        g_error_free(error);
                }
                nhlog::ui()->error(strError);
                if (errorMessage)
                        *errorMessage = strError;
                return false;
        }

        initialised_   = true;
        gchar *version = gst_version_string();
        nhlog::ui()->info("WebRTC: initialised {}", version);
        g_free(version);
        devices_.init();
        return true;
#else
        (void)errorMessage;
        return false;
#endif
}

#ifdef GSTREAMER_AVAILABLE
namespace {

std::string localsdp_;
std::vector<mtx::events::msg::CallCandidates::Candidate> localcandidates_;
bool haveAudioStream_;
bool haveVideoStream_;
GstPad *insetSinkPad_ = nullptr;

gboolean
newBusMessage(GstBus *bus G_GNUC_UNUSED, GstMessage *msg, gpointer user_data)
{
        WebRTCSession *session = static_cast<WebRTCSession *>(user_data);
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
                nhlog::ui()->error("WebRTC: end of stream");
                session->end();
                break;
        case GST_MESSAGE_ERROR:
                GError *error;
                gchar *debug;
                gst_message_parse_error(msg, &error, &debug);
                nhlog::ui()->error(
                  "WebRTC: error from element {}: {}", GST_OBJECT_NAME(msg->src), error->message);
                g_clear_error(&error);
                g_free(debug);
                session->end();
                break;
        default:
                break;
        }
        return TRUE;
}

GstWebRTCSessionDescription *
parseSDP(const std::string &sdp, GstWebRTCSDPType type)
{
        GstSDPMessage *msg;
        gst_sdp_message_new(&msg);
        if (gst_sdp_message_parse_buffer((guint8 *)sdp.c_str(), sdp.size(), msg) == GST_SDP_OK) {
                return gst_webrtc_session_description_new(type, msg);
        } else {
                nhlog::ui()->error("WebRTC: failed to parse remote session description");
                gst_sdp_message_free(msg);
                return nullptr;
        }
}

void
setLocalDescription(GstPromise *promise, gpointer webrtc)
{
        const GstStructure *reply = gst_promise_get_reply(promise);
        gboolean isAnswer = gst_structure_id_has_field(reply, g_quark_from_string("answer"));
        GstWebRTCSessionDescription *gstsdp = nullptr;
        gst_structure_get(reply,
                          isAnswer ? "answer" : "offer",
                          GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                          &gstsdp,
                          nullptr);
        gst_promise_unref(promise);
        g_signal_emit_by_name(webrtc, "set-local-description", gstsdp, nullptr);

        gchar *sdp = gst_sdp_message_as_text(gstsdp->sdp);
        localsdp_  = std::string(sdp);
        g_free(sdp);
        gst_webrtc_session_description_free(gstsdp);

        nhlog::ui()->debug(
          "WebRTC: local description set ({}):\n{}", isAnswer ? "answer" : "offer", localsdp_);
}

void
createOffer(GstElement *webrtc)
{
        // create-offer first, then set-local-description
        GstPromise *promise =
          gst_promise_new_with_change_func(setLocalDescription, webrtc, nullptr);
        g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
}

void
createAnswer(GstPromise *promise, gpointer webrtc)
{
        // create-answer first, then set-local-description
        gst_promise_unref(promise);
        promise = gst_promise_new_with_change_func(setLocalDescription, webrtc, nullptr);
        g_signal_emit_by_name(webrtc, "create-answer", nullptr, promise);
}

#if GST_CHECK_VERSION(1, 18, 0)
void
iceGatheringStateChanged(GstElement *webrtc,
                         GParamSpec *pspec G_GNUC_UNUSED,
                         gpointer user_data G_GNUC_UNUSED)
{
        GstWebRTCICEGatheringState newState;
        g_object_get(webrtc, "ice-gathering-state", &newState, nullptr);
        if (newState == GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) {
                nhlog::ui()->debug("WebRTC: GstWebRTCICEGatheringState -> Complete");
                if (WebRTCSession::instance().isOffering()) {
                        emit WebRTCSession::instance().offerCreated(localsdp_, localcandidates_);
                        emit WebRTCSession::instance().stateChanged(State::OFFERSENT);
                } else {
                        emit WebRTCSession::instance().answerCreated(localsdp_, localcandidates_);
                        emit WebRTCSession::instance().stateChanged(State::ANSWERSENT);
                }
        }
}

#else

gboolean
onICEGatheringCompletion(gpointer timerid)
{
        *(guint *)(timerid) = 0;
        if (WebRTCSession::instance().isOffering()) {
                emit WebRTCSession::instance().offerCreated(localsdp_, localcandidates_);
                emit WebRTCSession::instance().stateChanged(State::OFFERSENT);
        } else {
                emit WebRTCSession::instance().answerCreated(localsdp_, localcandidates_);
                emit WebRTCSession::instance().stateChanged(State::ANSWERSENT);
        }
        return FALSE;
}
#endif

void
addLocalICECandidate(GstElement *webrtc G_GNUC_UNUSED,
                     guint mlineIndex,
                     gchar *candidate,
                     gpointer G_GNUC_UNUSED)
{
        nhlog::ui()->debug("WebRTC: local candidate: (m-line:{}):{}", mlineIndex, candidate);

#if GST_CHECK_VERSION(1, 18, 0)
        localcandidates_.push_back({std::string() /*max-bundle*/, (uint16_t)mlineIndex, candidate});
        return;
#else
        if (WebRTCSession::instance().state() >= State::OFFERSENT) {
                emit WebRTCSession::instance().newICECandidate(
                  {std::string() /*max-bundle*/, (uint16_t)mlineIndex, candidate});
                return;
        }

        localcandidates_.push_back({std::string() /*max-bundle*/, (uint16_t)mlineIndex, candidate});

        // GStreamer v1.16: webrtcbin's notify::ice-gathering-state triggers
        // GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE too early. Fixed in v1.18.
        // Use a 1s timeout in the meantime
        static guint timerid = 0;
        if (timerid)
                g_source_remove(timerid);

        timerid = g_timeout_add(1000, onICEGatheringCompletion, &timerid);
#endif
}

void
iceConnectionStateChanged(GstElement *webrtc,
                          GParamSpec *pspec G_GNUC_UNUSED,
                          gpointer user_data G_GNUC_UNUSED)
{
        GstWebRTCICEConnectionState newState;
        g_object_get(webrtc, "ice-connection-state", &newState, nullptr);
        switch (newState) {
        case GST_WEBRTC_ICE_CONNECTION_STATE_CHECKING:
                nhlog::ui()->debug("WebRTC: GstWebRTCICEConnectionState -> Checking");
                emit WebRTCSession::instance().stateChanged(State::CONNECTING);
                break;
        case GST_WEBRTC_ICE_CONNECTION_STATE_FAILED:
                nhlog::ui()->error("WebRTC: GstWebRTCICEConnectionState -> Failed");
                emit WebRTCSession::instance().stateChanged(State::ICEFAILED);
                break;
        default:
                break;
        }
}

// https://gitlab.freedesktop.org/gstreamer/gst-plugins-bad/-/issues/1164
struct KeyFrameRequestData
{
        GstElement *pipe      = nullptr;
        GstElement *decodebin = nullptr;
        gint packetsLost      = 0;
        guint timerid         = 0;
        std::string statsField;
} keyFrameRequestData_;

void
sendKeyFrameRequest()
{
        GstPad *sinkpad = gst_element_get_static_pad(keyFrameRequestData_.decodebin, "sink");
        if (!gst_pad_push_event(sinkpad,
                                gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM,
                                                     gst_structure_new_empty("GstForceKeyUnit"))))
                nhlog::ui()->error("WebRTC: key frame request failed");
        else
                nhlog::ui()->debug("WebRTC: sent key frame request");

        gst_object_unref(sinkpad);
}

void
testPacketLoss_(GstPromise *promise, gpointer G_GNUC_UNUSED)
{
        const GstStructure *reply = gst_promise_get_reply(promise);
        gint packetsLost          = 0;
        GstStructure *rtpStats;
        if (!gst_structure_get(reply,
                               keyFrameRequestData_.statsField.c_str(),
                               GST_TYPE_STRUCTURE,
                               &rtpStats,
                               nullptr)) {
                nhlog::ui()->error("WebRTC: get-stats: no field: {}",
                                   keyFrameRequestData_.statsField);
                gst_promise_unref(promise);
                return;
        }
        gst_structure_get_int(rtpStats, "packets-lost", &packetsLost);
        gst_structure_free(rtpStats);
        gst_promise_unref(promise);
        if (packetsLost > keyFrameRequestData_.packetsLost) {
                nhlog::ui()->debug("WebRTC: inbound video lost packet count: {}", packetsLost);
                keyFrameRequestData_.packetsLost = packetsLost;
                sendKeyFrameRequest();
        }
}

gboolean
testPacketLoss(gpointer G_GNUC_UNUSED)
{
        if (keyFrameRequestData_.pipe) {
                GstElement *webrtc =
                  gst_bin_get_by_name(GST_BIN(keyFrameRequestData_.pipe), "webrtcbin");
                GstPromise *promise =
                  gst_promise_new_with_change_func(testPacketLoss_, nullptr, nullptr);
                g_signal_emit_by_name(webrtc, "get-stats", nullptr, promise);
                gst_object_unref(webrtc);
                return TRUE;
        }
        return FALSE;
}

#if GST_CHECK_VERSION(1, 18, 0)
void
setWaitForKeyFrame(GstBin *decodebin G_GNUC_UNUSED, GstElement *element, gpointer G_GNUC_UNUSED)
{
        if (!std::strcmp(
              gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(gst_element_get_factory(element))),
              "rtpvp8depay"))
                g_object_set(element, "wait-for-keyframe", TRUE, nullptr);
}
#endif

GstElement *
newAudioSinkChain(GstElement *pipe)
{
        GstElement *queue    = gst_element_factory_make("queue", nullptr);
        GstElement *convert  = gst_element_factory_make("audioconvert", nullptr);
        GstElement *resample = gst_element_factory_make("audioresample", nullptr);
        GstElement *sink     = gst_element_factory_make("autoaudiosink", nullptr);
        gst_bin_add_many(GST_BIN(pipe), queue, convert, resample, sink, nullptr);
        gst_element_link_many(queue, convert, resample, sink, nullptr);
        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(convert);
        gst_element_sync_state_with_parent(resample);
        gst_element_sync_state_with_parent(sink);
        return queue;
}

GstElement *
newVideoSinkChain(GstElement *pipe)
{
        // use compositor for now; acceleration needs investigation
        GstElement *queue          = gst_element_factory_make("queue", nullptr);
        GstElement *compositor     = gst_element_factory_make("compositor", "compositor");
        GstElement *glupload       = gst_element_factory_make("glupload", nullptr);
        GstElement *glcolorconvert = gst_element_factory_make("glcolorconvert", nullptr);
        GstElement *qmlglsink      = gst_element_factory_make("qmlglsink", nullptr);
        GstElement *glsinkbin      = gst_element_factory_make("glsinkbin", nullptr);
        g_object_set(qmlglsink, "widget", WebRTCSession::instance().getVideoItem(), nullptr);
        g_object_set(glsinkbin, "sink", qmlglsink, nullptr);
        gst_bin_add_many(
          GST_BIN(pipe), queue, compositor, glupload, glcolorconvert, glsinkbin, nullptr);
        gst_element_link_many(queue, compositor, glupload, glcolorconvert, glsinkbin, nullptr);
        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(compositor);
        gst_element_sync_state_with_parent(glupload);
        gst_element_sync_state_with_parent(glcolorconvert);
        gst_element_sync_state_with_parent(glsinkbin);
        return queue;
}

std::pair<int, int>
getResolution(GstPad *pad)
{
        std::pair<int, int> ret;
        GstCaps *caps         = gst_pad_get_current_caps(pad);
        const GstStructure *s = gst_caps_get_structure(caps, 0);
        gst_structure_get_int(s, "width", &ret.first);
        gst_structure_get_int(s, "height", &ret.second);
        gst_caps_unref(caps);
        return ret;
}

void
addCameraView(GstElement *pipe, const std::pair<int, int> &videoCallSize)
{
        GstElement *tee = gst_bin_get_by_name(GST_BIN(pipe), "videosrctee");
        if (!tee)
                return;

        GstElement *queue     = gst_element_factory_make("queue", nullptr);
        GstElement *videorate = gst_element_factory_make("videorate", nullptr);
        gst_bin_add_many(GST_BIN(pipe), queue, videorate, nullptr);
        gst_element_link_many(tee, queue, videorate, nullptr);
        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(videorate);
        gst_object_unref(tee);

        GstElement *camerafilter = gst_bin_get_by_name(GST_BIN(pipe), "camerafilter");
        GstPad *filtersinkpad    = gst_element_get_static_pad(camerafilter, "sink");
        auto cameraResolution    = getResolution(filtersinkpad);
        int insetWidth           = videoCallSize.first / 4;
        int insetHeight =
          static_cast<double>(cameraResolution.second) / cameraResolution.first * insetWidth;
        nhlog::ui()->debug("WebRTC: picture-in-picture size: {}x{}", insetWidth, insetHeight);
        gst_object_unref(filtersinkpad);
        gst_object_unref(camerafilter);

        GstPad *camerapad      = gst_element_get_static_pad(videorate, "src");
        GstElement *compositor = gst_bin_get_by_name(GST_BIN(pipe), "compositor");
        insetSinkPad_          = gst_element_get_request_pad(compositor, "sink_%u");
        g_object_set(insetSinkPad_, "zorder", 2, nullptr);
        g_object_set(insetSinkPad_, "width", insetWidth, "height", insetHeight, nullptr);
        gint offset = videoCallSize.first / 80;
        g_object_set(insetSinkPad_, "xpos", offset, "ypos", offset, nullptr);
        if (GST_PAD_LINK_FAILED(gst_pad_link(camerapad, insetSinkPad_)))
                nhlog::ui()->error("WebRTC: failed to link camera view chain");
        gst_object_unref(camerapad);
        gst_object_unref(compositor);
}

void
linkNewPad(GstElement *decodebin, GstPad *newpad, GstElement *pipe)
{
        GstPad *sinkpad               = gst_element_get_static_pad(decodebin, "sink");
        GstCaps *sinkcaps             = gst_pad_get_current_caps(sinkpad);
        const GstStructure *structure = gst_caps_get_structure(sinkcaps, 0);

        gchar *mediaType = nullptr;
        guint ssrc       = 0;
        gst_structure_get(
          structure, "media", G_TYPE_STRING, &mediaType, "ssrc", G_TYPE_UINT, &ssrc, nullptr);
        gst_caps_unref(sinkcaps);
        gst_object_unref(sinkpad);

        WebRTCSession *session = &WebRTCSession::instance();
        GstElement *queue      = nullptr;
        if (!std::strcmp(mediaType, "audio")) {
                nhlog::ui()->debug("WebRTC: received incoming audio stream");
                haveAudioStream_ = true;
                queue            = newAudioSinkChain(pipe);
        } else if (!std::strcmp(mediaType, "video")) {
                nhlog::ui()->debug("WebRTC: received incoming video stream");
                if (!session->getVideoItem()) {
                        g_free(mediaType);
                        nhlog::ui()->error("WebRTC: video call item not set");
                        return;
                }
                haveVideoStream_ = true;
                keyFrameRequestData_.statsField =
                  std::string("rtp-inbound-stream-stats_") + std::to_string(ssrc);
                queue              = newVideoSinkChain(pipe);
                auto videoCallSize = getResolution(newpad);
                nhlog::ui()->info("WebRTC: incoming video resolution: {}x{}",
                                  videoCallSize.first,
                                  videoCallSize.second);
                addCameraView(pipe, videoCallSize);
        } else {
                g_free(mediaType);
                nhlog::ui()->error("WebRTC: unknown pad type: {}", GST_PAD_NAME(newpad));
                return;
        }

        GstPad *queuepad = gst_element_get_static_pad(queue, "sink");
        if (queuepad) {
                if (GST_PAD_LINK_FAILED(gst_pad_link(newpad, queuepad)))
                        nhlog::ui()->error("WebRTC: unable to link new pad");
                else {
                        if (!session->isVideo() ||
                            (haveAudioStream_ &&
                             (haveVideoStream_ || session->isRemoteVideoRecvOnly()))) {
                                emit session->stateChanged(State::CONNECTED);
                                if (haveVideoStream_) {
                                        keyFrameRequestData_.pipe      = pipe;
                                        keyFrameRequestData_.decodebin = decodebin;
                                        keyFrameRequestData_.timerid =
                                          g_timeout_add_seconds(3, testPacketLoss, nullptr);
                                }
                        }
                }
                gst_object_unref(queuepad);
        }
        g_free(mediaType);
}

void
addDecodeBin(GstElement *webrtc G_GNUC_UNUSED, GstPad *newpad, GstElement *pipe)
{
        if (GST_PAD_DIRECTION(newpad) != GST_PAD_SRC)
                return;

        nhlog::ui()->debug("WebRTC: received incoming stream");
        GstElement *decodebin = gst_element_factory_make("decodebin", nullptr);
        // hardware decoding needs investigation; eg rendering fails if vaapi plugin installed
        g_object_set(decodebin, "force-sw-decoders", TRUE, nullptr);
        g_signal_connect(decodebin, "pad-added", G_CALLBACK(linkNewPad), pipe);
#if GST_CHECK_VERSION(1, 18, 0)
        g_signal_connect(decodebin, "element-added", G_CALLBACK(setWaitForKeyFrame), nullptr);
#endif
        gst_bin_add(GST_BIN(pipe), decodebin);
        gst_element_sync_state_with_parent(decodebin);
        GstPad *sinkpad = gst_element_get_static_pad(decodebin, "sink");
        if (GST_PAD_LINK_FAILED(gst_pad_link(newpad, sinkpad)))
                nhlog::ui()->error("WebRTC: unable to link decodebin");
        gst_object_unref(sinkpad);
}

bool
contains(std::string_view str1, std::string_view str2)
{
        return std::search(str1.cbegin(),
                           str1.cend(),
                           str2.cbegin(),
                           str2.cend(),
                           [](unsigned char c1, unsigned char c2) {
                                   return std::tolower(c1) == std::tolower(c2);
                           }) != str1.cend();
}

bool
getMediaAttributes(const GstSDPMessage *sdp,
                   const char *mediaType,
                   const char *encoding,
                   int &payloadType,
                   bool &recvOnly)
{
        payloadType = -1;
        recvOnly    = false;
        for (guint mlineIndex = 0; mlineIndex < gst_sdp_message_medias_len(sdp); ++mlineIndex) {
                const GstSDPMedia *media = gst_sdp_message_get_media(sdp, mlineIndex);
                if (!std::strcmp(gst_sdp_media_get_media(media), mediaType)) {
                        recvOnly = gst_sdp_media_get_attribute_val(media, "recvonly") != nullptr;
                        const gchar *rtpval = nullptr;
                        for (guint n = 0; n == 0 || rtpval; ++n) {
                                rtpval = gst_sdp_media_get_attribute_val_n(media, "rtpmap", n);
                                if (rtpval && contains(rtpval, encoding)) {
                                        payloadType = std::atoi(rtpval);
                                        break;
                                }
                        }
                        return true;
                }
        }
        return false;
}

}

bool
WebRTCSession::havePlugins(bool isVideo, std::string *errorMessage)
{
        if (!initialised_ && !init(errorMessage))
                return false;
        if (!isVideo && haveVoicePlugins_)
                return true;
        if (isVideo && haveVideoPlugins_)
                return true;

        const gchar *voicePlugins[] = {"audioconvert",
                                       "audioresample",
                                       "autodetect",
                                       "dtls",
                                       "nice",
                                       "opus",
                                       "playback",
                                       "rtpmanager",
                                       "srtp",
                                       "volume",
                                       "webrtc",
                                       nullptr};

        const gchar *videoPlugins[] = {
          "compositor", "opengl", "qmlgl", "rtp", "videoconvert", "vpx", nullptr};

        std::string strError("Missing GStreamer plugins: ");
        const gchar **needed  = isVideo ? videoPlugins : voicePlugins;
        bool &havePlugins     = isVideo ? haveVideoPlugins_ : haveVoicePlugins_;
        havePlugins           = true;
        GstRegistry *registry = gst_registry_get();
        for (guint i = 0; i < g_strv_length((gchar **)needed); i++) {
                GstPlugin *plugin = gst_registry_find_plugin(registry, needed[i]);
                if (!plugin) {
                        havePlugins = false;
                        strError += std::string(needed[i]) + " ";
                        continue;
                }
                gst_object_unref(plugin);
        }
        if (!havePlugins) {
                nhlog::ui()->error(strError);
                if (errorMessage)
                        *errorMessage = strError;
                return false;
        }

        if (isVideo) {
                // load qmlglsink to register GStreamer's GstGLVideoItem QML type
                GstElement *qmlglsink = gst_element_factory_make("qmlglsink", nullptr);
                gst_object_unref(qmlglsink);
        }
        return true;
}

bool
WebRTCSession::createOffer(bool isVideo)
{
        isOffering_            = true;
        isVideo_               = isVideo;
        isRemoteVideoRecvOnly_ = false;
        videoItem_             = nullptr;
        haveAudioStream_       = false;
        haveVideoStream_       = false;
        insetSinkPad_          = nullptr;
        localsdp_.clear();
        localcandidates_.clear();

        // opus and vp8 rtp payload types must be defined dynamically
        // therefore from the range [96-127]
        // see for example https://tools.ietf.org/html/rfc7587
        constexpr int opusPayloadType = 111;
        constexpr int vp8PayloadType  = 96;
        return startPipeline(opusPayloadType, vp8PayloadType);
}

bool
WebRTCSession::acceptOffer(const std::string &sdp)
{
        nhlog::ui()->debug("WebRTC: received offer:\n{}", sdp);
        if (state_ != State::DISCONNECTED)
                return false;

        isOffering_            = false;
        isRemoteVideoRecvOnly_ = false;
        videoItem_             = nullptr;
        haveAudioStream_       = false;
        haveVideoStream_       = false;
        insetSinkPad_          = nullptr;
        localsdp_.clear();
        localcandidates_.clear();

        GstWebRTCSessionDescription *offer = parseSDP(sdp, GST_WEBRTC_SDP_TYPE_OFFER);
        if (!offer)
                return false;

        int opusPayloadType;
        bool recvOnly;
        if (getMediaAttributes(offer->sdp, "audio", "opus", opusPayloadType, recvOnly)) {
                if (opusPayloadType == -1) {
                        nhlog::ui()->error("WebRTC: remote audio offer - no opus encoding");
                        gst_webrtc_session_description_free(offer);
                        return false;
                }
        } else {
                nhlog::ui()->error("WebRTC: remote offer - no audio media");
                gst_webrtc_session_description_free(offer);
                return false;
        }

        int vp8PayloadType;
        isVideo_ =
          getMediaAttributes(offer->sdp, "video", "vp8", vp8PayloadType, isRemoteVideoRecvOnly_);
        if (isVideo_ && vp8PayloadType == -1) {
                nhlog::ui()->error("WebRTC: remote video offer - no vp8 encoding");
                gst_webrtc_session_description_free(offer);
                return false;
        }

        if (!startPipeline(opusPayloadType, vp8PayloadType)) {
                gst_webrtc_session_description_free(offer);
                return false;
        }

        // avoid a race that sometimes leaves the generated answer without media tracks (a=ssrc
        // lines)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // set-remote-description first, then create-answer
        GstPromise *promise = gst_promise_new_with_change_func(createAnswer, webrtc_, nullptr);
        g_signal_emit_by_name(webrtc_, "set-remote-description", offer, promise);
        gst_webrtc_session_description_free(offer);
        return true;
}

bool
WebRTCSession::acceptAnswer(const std::string &sdp)
{
        nhlog::ui()->debug("WebRTC: received answer:\n{}", sdp);
        if (state_ != State::OFFERSENT)
                return false;

        GstWebRTCSessionDescription *answer = parseSDP(sdp, GST_WEBRTC_SDP_TYPE_ANSWER);
        if (!answer) {
                end();
                return false;
        }

        if (isVideo_) {
                int unused;
                if (!getMediaAttributes(
                      answer->sdp, "video", "vp8", unused, isRemoteVideoRecvOnly_))
                        isRemoteVideoRecvOnly_ = true;
        }

        g_signal_emit_by_name(webrtc_, "set-remote-description", answer, nullptr);
        gst_webrtc_session_description_free(answer);
        return true;
}

void
WebRTCSession::acceptICECandidates(
  const std::vector<mtx::events::msg::CallCandidates::Candidate> &candidates)
{
        if (state_ >= State::INITIATED) {
                for (const auto &c : candidates) {
                        nhlog::ui()->debug(
                          "WebRTC: remote candidate: (m-line:{}):{}", c.sdpMLineIndex, c.candidate);
                        if (!c.candidate.empty()) {
                                g_signal_emit_by_name(webrtc_,
                                                      "add-ice-candidate",
                                                      c.sdpMLineIndex,
                                                      c.candidate.c_str());
                        }
                }
        }
}

bool
WebRTCSession::startPipeline(int opusPayloadType, int vp8PayloadType)
{
        if (state_ != State::DISCONNECTED)
                return false;

        emit stateChanged(State::INITIATING);

        if (!createPipeline(opusPayloadType, vp8PayloadType)) {
                end();
                return false;
        }

        webrtc_ = gst_bin_get_by_name(GST_BIN(pipe_), "webrtcbin");

        if (ChatPage::instance()->userSettings()->useStunServer()) {
                nhlog::ui()->info("WebRTC: setting STUN server: {}", STUN_SERVER);
                g_object_set(webrtc_, "stun-server", STUN_SERVER, nullptr);
        }

        for (const auto &uri : turnServers_) {
                nhlog::ui()->info("WebRTC: setting TURN server: {}", uri);
                gboolean udata;
                g_signal_emit_by_name(webrtc_, "add-turn-server", uri.c_str(), (gpointer)(&udata));
        }
        if (turnServers_.empty())
                nhlog::ui()->warn("WebRTC: no TURN server provided");

        // generate the offer when the pipeline goes to PLAYING
        if (isOffering_)
                g_signal_connect(
                  webrtc_, "on-negotiation-needed", G_CALLBACK(::createOffer), nullptr);

        // on-ice-candidate is emitted when a local ICE candidate has been gathered
        g_signal_connect(webrtc_, "on-ice-candidate", G_CALLBACK(addLocalICECandidate), nullptr);

        // capture ICE failure
        g_signal_connect(
          webrtc_, "notify::ice-connection-state", G_CALLBACK(iceConnectionStateChanged), nullptr);

        // incoming streams trigger pad-added
        gst_element_set_state(pipe_, GST_STATE_READY);
        g_signal_connect(webrtc_, "pad-added", G_CALLBACK(addDecodeBin), pipe_);

#if GST_CHECK_VERSION(1, 18, 0)
        // capture ICE gathering completion
        g_signal_connect(
          webrtc_, "notify::ice-gathering-state", G_CALLBACK(iceGatheringStateChanged), nullptr);
#endif
        // webrtcbin lifetime is the same as that of the pipeline
        gst_object_unref(webrtc_);

        // start the pipeline
        GstStateChangeReturn ret = gst_element_set_state(pipe_, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
                nhlog::ui()->error("WebRTC: unable to start pipeline");
                end();
                return false;
        }

        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipe_));
        busWatchId_ = gst_bus_add_watch(bus, newBusMessage, this);
        gst_object_unref(bus);
        emit stateChanged(State::INITIATED);
        return true;
}

bool
WebRTCSession::createPipeline(int opusPayloadType, int vp8PayloadType)
{
        GstDevice *device = devices_.audioDevice();
        if (!device)
                return false;

        GstElement *source     = gst_device_create_element(device, nullptr);
        GstElement *volume     = gst_element_factory_make("volume", "srclevel");
        GstElement *convert    = gst_element_factory_make("audioconvert", nullptr);
        GstElement *resample   = gst_element_factory_make("audioresample", nullptr);
        GstElement *queue1     = gst_element_factory_make("queue", nullptr);
        GstElement *opusenc    = gst_element_factory_make("opusenc", nullptr);
        GstElement *rtp        = gst_element_factory_make("rtpopuspay", nullptr);
        GstElement *queue2     = gst_element_factory_make("queue", nullptr);
        GstElement *capsfilter = gst_element_factory_make("capsfilter", nullptr);

        GstCaps *rtpcaps = gst_caps_new_simple("application/x-rtp",
                                               "media",
                                               G_TYPE_STRING,
                                               "audio",
                                               "encoding-name",
                                               G_TYPE_STRING,
                                               "OPUS",
                                               "payload",
                                               G_TYPE_INT,
                                               opusPayloadType,
                                               nullptr);
        g_object_set(capsfilter, "caps", rtpcaps, nullptr);
        gst_caps_unref(rtpcaps);

        GstElement *webrtcbin = gst_element_factory_make("webrtcbin", "webrtcbin");
        g_object_set(webrtcbin, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);

        pipe_ = gst_pipeline_new(nullptr);
        gst_bin_add_many(GST_BIN(pipe_),
                         source,
                         volume,
                         convert,
                         resample,
                         queue1,
                         opusenc,
                         rtp,
                         queue2,
                         capsfilter,
                         webrtcbin,
                         nullptr);

        if (!gst_element_link_many(source,
                                   volume,
                                   convert,
                                   resample,
                                   queue1,
                                   opusenc,
                                   rtp,
                                   queue2,
                                   capsfilter,
                                   webrtcbin,
                                   nullptr)) {
                nhlog::ui()->error("WebRTC: failed to link audio pipeline elements");
                return false;
        }

        return isVideo_ ? addVideoPipeline(vp8PayloadType) : true;
}

bool
WebRTCSession::addVideoPipeline(int vp8PayloadType)
{
        // allow incoming video calls despite localUser having no webcam
        if (!devices_.haveCamera())
                return !isOffering_;

        std::pair<int, int> resolution;
        std::pair<int, int> frameRate;
        GstDevice *device = devices_.videoDevice(resolution, frameRate);
        if (!device)
                return false;

        GstElement *source       = gst_device_create_element(device, nullptr);
        GstElement *videoconvert = gst_element_factory_make("videoconvert", nullptr);
        GstElement *capsfilter   = gst_element_factory_make("capsfilter", "camerafilter");
        GstCaps *caps            = gst_caps_new_simple("video/x-raw",
                                            "width",
                                            G_TYPE_INT,
                                            resolution.first,
                                            "height",
                                            G_TYPE_INT,
                                            resolution.second,
                                            "framerate",
                                            GST_TYPE_FRACTION,
                                            frameRate.first,
                                            frameRate.second,
                                            nullptr);
        g_object_set(capsfilter, "caps", caps, nullptr);
        gst_caps_unref(caps);
        GstElement *tee    = gst_element_factory_make("tee", "videosrctee");
        GstElement *queue  = gst_element_factory_make("queue", nullptr);
        GstElement *vp8enc = gst_element_factory_make("vp8enc", nullptr);
        g_object_set(vp8enc, "deadline", 1, nullptr);
        g_object_set(vp8enc, "error-resilient", 1, nullptr);
        GstElement *rtpvp8pay     = gst_element_factory_make("rtpvp8pay", nullptr);
        GstElement *rtpqueue      = gst_element_factory_make("queue", nullptr);
        GstElement *rtpcapsfilter = gst_element_factory_make("capsfilter", nullptr);
        GstCaps *rtpcaps          = gst_caps_new_simple("application/x-rtp",
                                               "media",
                                               G_TYPE_STRING,
                                               "video",
                                               "encoding-name",
                                               G_TYPE_STRING,
                                               "VP8",
                                               "payload",
                                               G_TYPE_INT,
                                               vp8PayloadType,
                                               nullptr);
        g_object_set(rtpcapsfilter, "caps", rtpcaps, nullptr);
        gst_caps_unref(rtpcaps);

        gst_bin_add_many(GST_BIN(pipe_),
                         source,
                         videoconvert,
                         capsfilter,
                         tee,
                         queue,
                         vp8enc,
                         rtpvp8pay,
                         rtpqueue,
                         rtpcapsfilter,
                         nullptr);

        GstElement *webrtcbin = gst_bin_get_by_name(GST_BIN(pipe_), "webrtcbin");
        if (!gst_element_link_many(source,
                                   videoconvert,
                                   capsfilter,
                                   tee,
                                   queue,
                                   vp8enc,
                                   rtpvp8pay,
                                   rtpqueue,
                                   rtpcapsfilter,
                                   webrtcbin,
                                   nullptr)) {
                nhlog::ui()->error("WebRTC: failed to link video pipeline elements");
                gst_object_unref(webrtcbin);
                return false;
        }
        gst_object_unref(webrtcbin);
        return true;
}

bool
WebRTCSession::haveLocalVideo() const
{
        if (isVideo_ && state_ >= State::INITIATED) {
                GstElement *tee = gst_bin_get_by_name(GST_BIN(pipe_), "videosrctee");
                if (tee) {
                        gst_object_unref(tee);
                        return true;
                }
        }
        return false;
}

bool
WebRTCSession::isMicMuted() const
{
        if (state_ < State::INITIATED)
                return false;

        GstElement *srclevel = gst_bin_get_by_name(GST_BIN(pipe_), "srclevel");
        gboolean muted;
        g_object_get(srclevel, "mute", &muted, nullptr);
        gst_object_unref(srclevel);
        return muted;
}

bool
WebRTCSession::toggleMicMute()
{
        if (state_ < State::INITIATED)
                return false;

        GstElement *srclevel = gst_bin_get_by_name(GST_BIN(pipe_), "srclevel");
        gboolean muted;
        g_object_get(srclevel, "mute", &muted, nullptr);
        g_object_set(srclevel, "mute", !muted, nullptr);
        gst_object_unref(srclevel);
        return !muted;
}

void
WebRTCSession::toggleCameraView()
{
        if (insetSinkPad_) {
                guint zorder;
                g_object_get(insetSinkPad_, "zorder", &zorder, nullptr);
                g_object_set(insetSinkPad_, "zorder", zorder ? 0 : 2, nullptr);
        }
}

void
WebRTCSession::end()
{
        nhlog::ui()->debug("WebRTC: ending session");
        keyFrameRequestData_ = KeyFrameRequestData{};
        if (pipe_) {
                gst_element_set_state(pipe_, GST_STATE_NULL);
                gst_object_unref(pipe_);
                pipe_ = nullptr;
                if (busWatchId_) {
                        g_source_remove(busWatchId_);
                        busWatchId_ = 0;
                }
        }

        webrtc_                = nullptr;
        isVideo_               = false;
        isOffering_            = false;
        isRemoteVideoRecvOnly_ = false;
        videoItem_             = nullptr;
        insetSinkPad_          = nullptr;
        if (state_ != State::DISCONNECTED)
                emit stateChanged(State::DISCONNECTED);
}

#else

bool
WebRTCSession::havePlugins(bool, std::string *)
{
        return false;
}

bool
WebRTCSession::haveLocalVideo() const
{
        return false;
}

bool
WebRTCSession::createOffer(bool)
{
        return false;
}

bool
WebRTCSession::acceptOffer(const std::string &)
{
        return false;
}

bool
WebRTCSession::acceptAnswer(const std::string &)
{
        return false;
}

void
WebRTCSession::acceptICECandidates(const std::vector<mtx::events::msg::CallCandidates::Candidate> &)
{}

bool
WebRTCSession::isMicMuted() const
{
        return false;
}

bool
WebRTCSession::toggleMicMute()
{
        return false;
}

void
WebRTCSession::toggleCameraView()
{}

void
WebRTCSession::end()
{}

#endif
