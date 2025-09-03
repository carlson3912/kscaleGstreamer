#define GST_USE_UNSTABLE_API

#include <stdio.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/webrtc/webrtc.h>
#include <gst/sdp/sdp.h>
#include <json-glib/json-glib.h>
#include <libsoup-3.0/libsoup/soup.h>
#include <glib.h>

#define VIDEO_SOURCE "/base/axi/pcie@1000120000/rp1/i2c@80000/ov5647@36"
#define VIDEO_SOURCE2 "/base/axi/pcie@1000120000/rp1/i2c@88000/ov5647@36"
#define STUN_SERVER "stun://stun.l.google.com:19302"

static GMainLoop *loop;
static GstElement *pipeline = NULL;
static GstElement *webrtc = NULL;
static SoupWebsocketConnection *ws_conn = NULL;
static gint added_streams = 0;

// Structure to track packet loss per transceiver
typedef struct {
    guint64 last_num_lost;
    GstWebRTCRTPTransceiver *transceiver;
    GstElement *encoder;  // Reference to the encoder element for keyframe requests
} LossTracker;

static GHashTable *loss_trackers = NULL;  // Maps transceiver pointer to LossTracker

// static void send_pli_for_transceiver(GstWebRTCRTPTransceiver *transceiver) {
//     if (!transceiver) return;

//     g_print("Sending PLI to remote peer\n");

//     GstPad *sink_pad = NULL;
//     g_object_get(transceiver, "sink-pad", &sink_pad, NULL);
//     if (!sink_pad) {
//         g_printerr("Failed to get transceiver sink pad\n");
//         return;
//     }

//     // Create a "GstForceKeyUnit" event (this triggers a PLI in 1.22 webrtcbin)
//     GstStructure *s = gst_structure_new_empty("GstForceKeyUnit");
//     GstEvent *event = gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, s);

//     if (!gst_pad_push_event(sink_pad, event)) {
//         g_printerr("Failed to push PLI event\n");
//     } else {
//         g_print("PLI event pushed successfully\n");
//     }

//     gst_object_unref(sink_pad);
// }

// static void handle_packet_loss(const gchar *ssrc_str, guint64 new_lost, LossTracker *tracker) {
//     if (new_lost > tracker->last_num_lost) {
//         guint64 loss_delta = new_lost - tracker->last_num_lost;
//         g_print("PACKET LOSS DETECTED for SSRC %s: %" G_GUINT64_FORMAT " new packets lost (total: %" G_GUINT64_FORMAT ")\n", 
//                 ssrc_str, loss_delta, new_lost);

//         // Send actual RTCP PLI to remote peer
//         if (tracker->transceiver) {
//             send_pli_for_transceiver(tracker->transceiver);
//         }

//         tracker->last_num_lost = new_lost;
//     }
// }

static void on_offer_created(GstPromise *promise, gpointer user_data) {
    GstPromiseResult result = gst_promise_wait(promise);
    // g_print("Promise wait result: %d\n", result);
    // if (result != GST_PROMISE_RESULT_REPLIED) {
    //     g_printerr("Promise failed with result: %d\n", result);
    //     gst_promise_unref(promise);
    //     return;
    // }

    const GstStructure *reply = gst_promise_get_reply(promise);
    // if (!reply) {
    //     g_printerr("No reply structure in promise\n");
    //     gst_promise_unref(promise);
    //     return;
    // }
    
    // // Debug: print the structure
    // gchar *struct_str = gst_structure_to_string(reply);
    // g_print("Promise reply structure: %s\n", struct_str);
    // g_free(struct_str);
    
    GstWebRTCSessionDescription *offer = NULL;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);

    if (!offer) {
        g_printerr("Failed to get SDP offer from structure\n");
        // Check if there's an error in the structure
        const GValue *error_val = gst_structure_get_value(reply, "error");
        if (error_val && G_VALUE_HOLDS(error_val, G_TYPE_ERROR)) {
            GError *error = g_value_get_boxed(error_val);
            g_printerr("Error in offer creation: %s\n", error->message);
        }
        gst_promise_unref(promise);
        return;
    }

    GstPromise *local_desc_promise = gst_promise_new();
    g_signal_emit_by_name(webrtc, "set-local-description", offer, local_desc_promise);
    gst_promise_interrupt(local_desc_promise);
    gst_promise_unref(local_desc_promise);

    gchar *sdp_text = gst_sdp_message_as_text(offer->sdp);

    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "sdp");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "offer");
    json_builder_set_member_name(builder, "sdp");
    json_builder_add_string_value(builder, sdp_text);
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    gchar *message = json_generator_to_data(gen, NULL);

    // Check if connection is still valid before sending
    if (ws_conn && soup_websocket_connection_get_state(ws_conn) == SOUP_WEBSOCKET_STATE_OPEN) {
        soup_websocket_connection_send_text(ws_conn, message);
        g_print("SDP offer sent\n");
    } else {
        g_printerr("WebSocket connection not available for sending offer\n");
    }

    g_free(message);
    g_free(sdp_text);
    g_object_unref(gen);
    json_node_unref(root);
    g_object_unref(builder);

    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);
}

static void on_negotiation_needed(GstElement *webrtcbin, gpointer user_data) {
    g_print("Negotiation needed - creating offer\n");
    
    // Add a small delay to ensure everything is fully ready
    g_usleep(100000); // 100ms delay
    
    GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, NULL, NULL);
    g_signal_emit_by_name(webrtcbin, "create-offer", NULL, promise);
    g_print("Create offer signal emitted\n");
}

static void on_ice_candidate(GstElement *webrtcbin, guint mlineindex, gchar *candidate, gpointer user_data) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "ice");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "candidate");
    json_builder_add_string_value(builder, candidate);
    json_builder_set_member_name(builder, "sdpMLineIndex");
    json_builder_add_int_value(builder, mlineindex);
    json_builder_end_object(builder);
    json_builder_end_object(builder);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(gen, root);
    gchar *message = json_generator_to_data(gen, NULL);

    // Check if connection is still valid before sending
    if (ws_conn && soup_websocket_connection_get_state(ws_conn) == SOUP_WEBSOCKET_STATE_OPEN) {
        soup_websocket_connection_send_text(ws_conn, message);
        g_print("ICE candidate sent\n");
    } else {
        g_printerr("WebSocket connection not available for sending ICE candidate\n");
    }

    g_free(message);
    g_object_unref(gen);
    json_node_unref(root);
    g_object_unref(builder);
}

static gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data) {
    switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old_state, new_state;
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(pipeline)) {
                gst_message_parse_state_changed(message, &old_state, &new_state, NULL);
                g_print("Pipeline state changed from %s to %s\n",
                        gst_element_state_get_name(old_state),
                        gst_element_state_get_name(new_state));
                
                // Only connect negotiation-needed after pipeline is playing
                if (new_state == GST_STATE_PLAYING && old_state != GST_STATE_PLAYING) {
                    g_print("Pipeline is now playing, connecting negotiation signal\n");
                    
                    // Add some debugging info about the webrtc element
                    GArray *transceivers;
                    g_signal_emit_by_name(webrtc, "get-transceivers", &transceivers);
                    
                    g_print("Number of transceivers: %d\n", transceivers ? transceivers->len : 0);
                    
                    // Enable NACK on all transceivers and initialize loss trackers
                    if (transceivers) {
                        for (guint i = 0; i < transceivers->len; i++) {
                            GstWebRTCRTPTransceiver *trans = g_array_index(transceivers, GstWebRTCRTPTransceiver *, i);
                            g_object_set(trans, "do-nack", TRUE, NULL);
                            g_object_set(trans, "fec-type", GST_WEBRTC_FEC_TYPE_ULP_RED, NULL);
                            g_print("Enabled NACK on transceiver %d\n", i);
                            
                            // Initialize loss tracker for this transceiver
                            LossTracker *tracker = g_new0(LossTracker, 1);
                            tracker->last_num_lost = 0;
                            tracker->transceiver = g_object_ref(trans);
                            tracker->encoder = NULL; // Will be set during pipeline creation
                            
                            g_hash_table_insert(loss_trackers, trans, tracker);
                        }
                        g_array_unref(transceivers);
                    }
                    
                    // Connect the negotiation signal now that pipeline is stable
                    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), NULL);
                }
            }
            break;
        }
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug_info;
            gst_message_parse_error(message, &err, &debug_info);
            g_printerr("Error from %s: %s\n", GST_OBJECT_NAME(message->src), err->message);
            g_printerr("Debug info: %s\n", debug_info ? debug_info : "none");
            g_error_free(err);
            g_free(debug_info);
            break;
        }
        case GST_MESSAGE_STREAM_START:
            g_print("Stream started\n");
            break;
        case GST_MESSAGE_ASYNC_DONE:
            g_print("Async done - pipeline is prerolled\n");
            break;
        default:
            break;
    }
    return TRUE;
}
static void on_incoming_stream(GstElement *webrtcbin, GstPad *pad, gpointer user_data) {
    if (gst_pad_get_direction(pad) != GST_PAD_SRC) {
        return;
    }

    g_print("New incoming stream pad: %s\n", gst_pad_get_name(pad));

    GstWebRTCRTPTransceiver *transceiver = NULL;
    g_object_get(pad, "transceiver", &transceiver, NULL);
    if (transceiver) {
        g_object_set(transceiver, "do-nack", TRUE, NULL);
        g_object_set(transceiver, "fec-type", GST_WEBRTC_FEC_TYPE_ULP_RED, NULL);
        g_print("Enabled NACK on incoming transceiver\n");
        g_object_unref(transceiver);
    }

    // --- Manually create depay + decoder + sink ---
    GstElement *depay = gst_element_factory_make("rtpvp8depay", NULL);
    GstElement *dec = gst_element_factory_make("vp8dec", NULL);
    GstElement *sink = gst_element_factory_make("autovideosink", NULL);

    if (!depay || !dec || !sink) {
        g_printerr("Failed to create manual decode elements\n");
        return;
    }

    // Set depay properties
    g_object_set(depay,
                 "request-keyframe", TRUE,
                 "wait-for-keyframe", TRUE,
                 NULL);

    gst_bin_add_many(GST_BIN(pipeline), depay, dec, sink, NULL);
    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(dec);
    gst_element_sync_state_with_parent(sink);

    // Link depay -> dec -> sink
    if (!gst_element_link_many(depay, dec, sink, NULL)) {
        g_printerr("Failed to link depay -> dec -> sink\n");
        return;
    }

    // Link pad from webrtc -> depay
    GstPad *sink_pad = gst_element_get_static_pad(depay, "sink");
    if (!sink_pad) {
        g_printerr("Failed to get depay sink pad\n");
        return;
    }

    GstPadLinkReturn ret = gst_pad_link(pad, sink_pad);
    g_print("Incoming pad linked to depay: %d\n", ret);
    gst_object_unref(sink_pad);

    added_streams++;
}

static void cleanup_loss_tracker(gpointer data) {
    LossTracker *tracker = (LossTracker *)data;
    if (tracker->transceiver) {
        g_object_unref(tracker->transceiver);
    }
    g_free(tracker);
}

static void start_pipeline(JsonArray *cameras, gboolean audio) {
    g_print("Starting pipeline\n");
    
    // Initialize loss trackers hash table
    if (loss_trackers) {
        g_hash_table_destroy(loss_trackers);
    }
    loss_trackers = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, cleanup_loss_tracker);
    
    // Create main pipeline
    pipeline = gst_pipeline_new("pipeline");
    
    // Create webrtc element
    webrtc = gst_element_factory_make("webrtcbin", "sendrecv");
    g_object_set(webrtc, 
                 "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
                 "stun-server", STUN_SERVER,
                 "latency", 200,
                 NULL);
    
    gst_bin_add(GST_BIN(pipeline), webrtc);
    
    // Add bus message handler to monitor pipeline state
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);
    
    // Connect ICE candidate signal immediately (this doesn't depend on negotiation)
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate), NULL);
    g_signal_connect(webrtc, "pad-added", G_CALLBACK(on_incoming_stream), NULL);
    
    // Get number of cameras
    guint num_cameras = json_array_get_length(cameras);
    
    // Create video sources for each camera
    for (guint i = 0; i < num_cameras; i++) {
        gint camera_id = json_array_get_int_element(cameras, i);
        gchar *src_name = g_strdup_printf("libcamrasrc%d", i);
        gchar *caps_name = g_strdup_printf("caps%d", i);
        gchar *conv_name = g_strdup_printf("conv%d", i);
        gchar *queue_name = g_strdup_printf("queue%d", i);
        gchar *enc_name = g_strdup_printf("vp8enc%d", i);
        gchar *pay_name = g_strdup_printf("pay%d", i);
        
        // Create elements
        GstElement *src = gst_element_factory_make("libcamerasrc", src_name);
        GstElement *capsfilter = gst_element_factory_make("capsfilter", caps_name);
        GstElement *conv = gst_element_factory_make("videoconvert", conv_name);
        GstElement *queue = gst_element_factory_make("queue", queue_name);
        GstElement *enc = gst_element_factory_make("vp8enc", enc_name);
        GstElement *pay = gst_element_factory_make("rtpvp8pay", pay_name);
        
        if (!src || !capsfilter || !conv || !queue || !enc || !pay) {
            g_printerr("Failed to create elements for camera %d\n", i);
            goto cleanup;
        }
        
        // Set properties
        if(i == 0) {
            g_print("Setting device to %s\n", VIDEO_SOURCE);
            g_object_set(src, "camera-name", VIDEO_SOURCE, NULL);
        } else {
            g_print("Setting device to %s\n", VIDEO_SOURCE2);
            g_object_set(src, "camera-name", VIDEO_SOURCE2, NULL);
        }
        
        GstCaps *caps = gst_caps_from_string("video/x-raw,format=YUY2,width=1280,height=720,framerate=30/1");
        g_object_set(capsfilter, "caps", caps, NULL);
        gst_caps_unref(caps);
        
        g_object_set(queue, 
                     "leaky", 2,
                     "max-size-buffers", 2,
                     NULL);
                     
        g_object_set(enc,
                     "deadline", 1,
                     "keyframe-max-dist", 30,
                     NULL);
                     
        g_object_set(pay, "pt", 96 + i, NULL);
        
        // Add elements to pipeline
        gst_bin_add_many(GST_BIN(pipeline), src, capsfilter, conv, queue, enc, pay, NULL);
        
        // Link elements: src -> caps -> conv -> queue -> enc -> pay
        if (!gst_element_link_many(src, capsfilter, conv, queue, enc, pay, NULL)) {
            g_printerr("Failed to link elements for camera %d\n", i);
            goto cleanup;
        }
        
        // Link to webrtc - use sequential sink pad numbering
        GstPad *src_pad = gst_element_get_static_pad(pay, "src");
        GstPad *sink_pad = gst_element_get_request_pad(webrtc, "sink_%u");
        
        
        if (!src_pad || !sink_pad) {
            g_printerr("Failed to get pads for camera %d\n", i);
        } else {
            GstPadLinkReturn ret = gst_pad_link(src_pad, sink_pad);
            g_print("Camera %d pad link result: %d (linked to %s)\n", i, ret, gst_pad_get_name(sink_pad));
            
            // Get the transceiver for this pad and update loss tracker with encoder reference
            GstWebRTCRTPTransceiver *transceiver = NULL;
            g_object_get(sink_pad, "transceiver", &transceiver, NULL);
            if (transceiver) {
                g_print("Camera %d transceiver created: %s\n", i, GST_OBJECT_NAME(transceiver));
                
                // Update loss tracker with encoder reference
                LossTracker *tracker = g_hash_table_lookup(loss_trackers, transceiver);
                if (!tracker) {
                    tracker = g_new0(LossTracker, 1);
                    tracker->last_num_lost = 0;
                    tracker->transceiver = g_object_ref(transceiver);
                    g_hash_table_insert(loss_trackers, transceiver, tracker);
                }
                tracker->encoder = enc; // Store reference to encoder for keyframe requests
                
                g_object_unref(transceiver);
            }
        }
        
        if (src_pad) gst_object_unref(src_pad);
        if (sink_pad) gst_object_unref(sink_pad);
        
        g_print("Camera %d encoding: V4L2 -> Caps -> Convert -> Queue -> VP8 -> RTP\n", i);
        
        // Cleanup names
        g_free(src_name);
        g_free(caps_name); 
        g_free(conv_name);
        g_free(queue_name);
        g_free(enc_name);
        g_free(pay_name);
    }
    
    // Set pipeline to playing
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        g_printerr("Failed to set pipeline to playing state\n");
        goto cleanup;
    }
    
    g_print("Pipeline started with %d cameras, waiting for PLAYING state before negotiation\n", num_cameras);
    return;
    
cleanup:
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
        webrtc = NULL;
    }
    if (loss_trackers) {
        g_hash_table_destroy(loss_trackers);
        loss_trackers = NULL;
    }
}

static void ws_message(SoupWebsocketConnection *connection, SoupWebsocketDataType type, GBytes *message, gpointer user_data) {
    g_print("=== ws_message CALLED! Type: %d ===\n", type);
    
    if (type != SOUP_WEBSOCKET_DATA_TEXT) {
        g_print("Received non-text message, ignoring\n");
        return;
    }
    
    gsize size;
    const gchar *data = g_bytes_get_data(message, &size);
    if (!data || size == 0) {
        g_printerr("Received empty message\n");
        return;
    }
    
    gchar *msg_str = g_strndup(data, size);
    g_print("Received message: %s\n", msg_str);
    
    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, msg_str, -1, &error)) {
        g_printerr("Failed to parse JSON: %s\n", error->message);
        g_error_free(error);
        g_free(msg_str);
        g_object_unref(parser);
        return;
    }
    
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_printerr("Invalid JSON structure\n");
        g_free(msg_str);
        g_object_unref(parser);
        return;
    }
    
    JsonObject *obj = json_node_get_object(root);

    if (json_object_has_member(obj, "sdp")) {
        g_print("Processing SDP message\n");
        JsonObject *sdp_obj = json_object_get_object_member(obj, "sdp");
        const gchar *type_str = json_object_get_string_member(sdp_obj, "type");
        if (g_strcmp0(type_str, "answer") == 0) {
            const gchar *sdp_text = json_object_get_string_member(sdp_obj, "sdp");
            GstSDPMessage *sdp_msg;
            if (gst_sdp_message_new(&sdp_msg) == GST_SDP_OK) {
                if (gst_sdp_message_parse_buffer((guint8*)sdp_text, strlen(sdp_text), sdp_msg) == GST_SDP_OK) {
                    GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp_msg);
                    GstPromise *promise = gst_promise_new();
                    g_signal_emit_by_name(webrtc, "set-remote-description", answer, promise);
                    gst_promise_interrupt(promise);
                    gst_promise_unref(promise);
                    gst_webrtc_session_description_free(answer);
                    g_print("Remote description set successfully\n");
                } else {
                    g_printerr("Failed to parse SDP buffer\n");
                    gst_sdp_message_free(sdp_msg);
                }
            }
        }
    } else if (json_object_has_member(obj, "ice")) {
        g_print("Processing ICE candidate\n");
        JsonObject *ice_obj = json_object_get_object_member(obj, "ice");
        const gchar *candidate = json_object_get_string_member(ice_obj, "candidate");
        int mlineindex = json_object_get_int_member(ice_obj, "sdpMLineIndex");
        g_signal_emit_by_name(webrtc, "add-ice-candidate", mlineindex, candidate);
        g_print("ICE candidate added\n");
    } else if (json_object_has_member(obj, "type")) {
        const gchar *msg_type = json_object_get_string_member(obj, "type");
        g_print("Processing message type: %s\n", msg_type);
        if (g_strcmp0(msg_type, "Negotiate") == 0 && !pipeline) {
            JsonArray *cameras_array = NULL;
            gboolean audio = FALSE;
            
            if (json_object_has_member(obj, "cameras")) {
                cameras_array = json_object_get_array_member(obj, "cameras");
            }
            if (json_object_has_member(obj, "audio")) {
                audio = json_object_get_boolean_member(obj, "audio");
            }
            
            start_pipeline(cameras_array, audio);
        }
    }

    g_free(msg_str);
    g_object_unref(parser);
    g_print("Message processing completed\n");
}

static void on_ws_closed(SoupWebsocketConnection *connection, gpointer user_data) {
    g_print("WebSocket connection closed.\n");
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
        webrtc = NULL;
    }
    if (loss_trackers) {
        g_hash_table_destroy(loss_trackers);
        loss_trackers = NULL;
    }
    if (ws_conn) {
        g_object_unref(ws_conn);
        ws_conn = NULL;
    }
}

static void on_ws_error(SoupWebsocketConnection *connection, GError *error, gpointer user_data) {
    g_printerr("WebSocket error: %s\n", error->message);
}

static void ws_connected(SoupServer *server,
    SoupServerMessage *msg,
    const char *path,
    SoupWebsocketConnection *connection,
    gpointer user_data) {
        g_print("Client connected\n");
        
        // Take a reference to keep the connection alive
        ws_conn = g_object_ref(connection);
        
        // Debug: Print connection info
        g_print("Connection path: %s\n", path);
        g_print("Connection state: %d\n", soup_websocket_connection_get_state(connection));
        
        // Connect signals
        g_signal_connect(connection, "error", G_CALLBACK(on_ws_error), NULL);
        g_signal_connect(connection, "closed", G_CALLBACK(on_ws_closed), NULL);
        g_signal_connect(connection, "message", G_CALLBACK(ws_message), NULL);
        
        g_print("All signals connected\n");
    }
     

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    SoupServer *server = soup_server_new(NULL);
    soup_server_add_websocket_handler(server, "/ws", NULL, NULL, ws_connected, NULL, NULL);

    GError *error = NULL;
    GSocketAddress *address = g_inet_socket_address_new(g_inet_address_new_any(G_SOCKET_FAMILY_IPV4), 8765);
    if (!soup_server_listen(server, address, 0, &error)) {
        g_printerr("Failed to start server: %s\n", error->message);
        g_error_free(error);
        g_object_unref(address);
        return 1;
    }
    g_object_unref(address);
    
    g_print("WebSocket server running on ws://0.0.0.0:8765\n");

    g_main_loop_run(loop);

    // Cleanup
    if (ws_conn) {
        g_object_unref(ws_conn);
    }
    if (loss_trackers) {
        g_hash_table_destroy(loss_trackers);
    }
    g_object_unref(server);
    g_main_loop_unref(loop);

    return 0;
}