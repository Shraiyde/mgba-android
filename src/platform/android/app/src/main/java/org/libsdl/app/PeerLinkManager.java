package org.libsdl.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Handler;
import android.os.Looper;
import android.content.Context;
import android.content.SharedPreferences;
import android.text.InputFilter;
import android.text.InputType;
import android.util.Base64;
import android.view.Gravity;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import org.eclipse.paho.client.mqttv3.IMqttActionListener;
import org.eclipse.paho.client.mqttv3.IMqttDeliveryToken;
import org.eclipse.paho.client.mqttv3.IMqttToken;
import org.eclipse.paho.client.mqttv3.MqttAsyncClient;
import org.eclipse.paho.client.mqttv3.MqttCallbackExtended;
import org.eclipse.paho.client.mqttv3.MqttConnectOptions;
import org.eclipse.paho.client.mqttv3.MqttMessage;
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence;
import org.json.JSONObject;
import org.webrtc.DataChannel;
import org.webrtc.IceCandidate;
import org.webrtc.MediaConstraints;
import org.webrtc.PeerConnection;
import org.webrtc.PeerConnectionFactory;
import org.webrtc.SdpObserver;
import org.webrtc.SessionDescription;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.MessageDigest;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.X509EncodedKeySpec;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;

import javax.crypto.Cipher;
import javax.crypto.KeyAgreement;
import javax.crypto.Mac;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * PIN rendezvous, WebRTC direct transport, and MQTT relay fallback.
 * MQTT carries only short-lived signaling and encrypted fallback frames.
 */
public final class PeerLinkManager {
    private static final String BROKER = "wss://broker.emqx.io:8084/mqtt";
    private static final String PREFS = "hns_peer_link";
    private static final String PREF_ROLE = "role";
    private static final String PREF_PIN = "pin";
    private static final String PREF_SESSION = "session";
    private static final int PBKDF2_ITERATIONS = 500000;

    private static Activity activity;
    private static PeerLinkManager instance;
    private static boolean webRtcInitialized;

    private final SecureRandom random = new SecureRandom();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable roomHeartbeat = new Runnable() {
        @Override public void run() {
            if (host && mqttConnected) {
                publishRoomAdvertisement();
                handler.postDelayed(this, 10000);
            }
        }
    };
    private final Runnable pcHeartbeat = new Runnable() {
        @Override public void run() {
            if (pcServerMode && mqttConnected && pcHelloSent) {
                publish(pcUpHeartbeatTopic(), new byte[0], 0, false);
                handler.postDelayed(this, 10000);
            }
        }
    };
    private final Runnable pcServerWatchdog = new Runnable() {
        @Override public void run() {
            if (pcServerMode) {
                if (accepted && lastPcServerHeartbeatAt != 0
                    && System.currentTimeMillis() - lastPcServerHeartbeatAt > 30000) {
                    accepted = false;
                    sessionKey = null;
                    lastPcServerHeartbeatAt = 0;
                    updateNativeConnection();
                    showStatus("PC server offline", "The PC server stopped responding. Waiting for it to reconnect…");
                }
                handler.postDelayed(this, 10000);
            }
        }
    };
    private final AtomicLong sendCounter = new AtomicLong(1);
    private final long[] receiveCounters = new long[] {0, 0};
    private final List<IceCandidate> pendingCandidates = new ArrayList<>();

    private MqttAsyncClient mqtt;
    private PeerConnectionFactory factory;
    private PeerConnection peerConnection;
    private DataChannel dataChannel;
    private AlertDialog statusDialog;
    private String pin;
    private String sessionId;
    private String salt;
    private String hostPublicKey;
    private String toHostTopic;
    private String toJoinTopic;
    private String relayHostTopic;
    private String relayJoinTopic;
    private boolean host;
    private boolean pcServerMode;
    private boolean pcHelloSent;
    private long lastPcServerHeartbeatAt;
    private String pcClientId;
    private boolean mqttConnected;
    private boolean retryEnabled;
    private boolean accepted;
    private boolean joinSent;
    private boolean directConnected;
    private boolean nativeConnected;
    private KeyPair ecdhKeyPair;
    private byte[] pinKey;
    private byte[] sessionKey;

    private PeerLinkManager() {}

    public static synchronized void initialize(Activity value) {
        activity = value;
        if (instance == null)
            instance = new PeerLinkManager();
        nativeInitialize();
    }

    public static void showPairingDialog() {
        final PeerLinkManager manager = instance;
        final Activity current = activity;
        if (manager == null || current == null)
            return;

        current.runOnUiThread(() -> manager.showPairingDialogOnUi());
    }

    private void showPairingDialogOnUi() {
        SharedPreferences prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String savedRole = prefs.getString(PREF_ROLE, null);
        String savedPin = prefs.getString(PREF_PIN, null);

        AlertDialog.Builder builder = new AlertDialog.Builder(activity)
            .setTitle("Heart & Soul Co-op")
            .setMessage("Connect the two phones with a four-digit PIN.")
            .setNegativeButton("Play Offline", (dialog, which) -> dialog.dismiss())
            .setNeutralButton("Join", (dialog, which) -> showJoinPinDialog());

        if ("pc".equals(savedRole) && savedPin != null) {
            builder.setPositiveButton("Reconnect to PC Server", (dialog, which) -> startPcServerJoin(savedPin));
        } else if ("host".equals(savedRole) && savedPin != null) {
            builder.setPositiveButton("Reconnect as Host", (dialog, which) -> startHost(savedPin, prefs.getString(PREF_SESSION, null)));
        } else if ("join".equals(savedRole) && savedPin != null) {
            builder.setPositiveButton("Reconnect", (dialog, which) -> startJoin(savedPin));
        } else {
            builder.setPositiveButton("PC Server", (dialog, which) -> showPcServerPinDialog());
        }

        AlertDialog dialog = builder.create();
        dialog.show();
        dialog.setOnCancelListener(d -> {});
    }

    private void showJoinPinDialog() {
        EditText pinInput = new EditText(activity);
        pinInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        pinInput.setFilters(new InputFilter[] { new InputFilter.LengthFilter(4) });
        pinInput.setGravity(Gravity.CENTER);
        pinInput.setTextSize(28);

        new AlertDialog.Builder(activity)
            .setTitle("Enter Petra's PIN")
            .setView(pinInput)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Connect", (dialog, which) -> {
                String entered = pinInput.getText().toString().trim();
                if (entered.length() == 4)
                    startJoin(entered);
                else
                    showMessage("Enter all four digits.");
            })
            .show();
    }

    private void showPcServerPinDialog() {
        EditText pinInput = new EditText(activity);
        pinInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        pinInput.setFilters(new InputFilter[] { new InputFilter.LengthFilter(4) });
        pinInput.setGravity(Gravity.CENTER);
        pinInput.setTextSize(28);

        new AlertDialog.Builder(activity)
            .setTitle("Enter the PC server PIN")
            .setView(pinInput)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Connect", (dialog, which) -> {
                String entered = pinInput.getText().toString().trim();
                if (entered.matches("\\d{4}"))
                    startPcServerJoin(entered);
                else
                    showMessage("Enter all four digits.");
            })
            .show();
    }

    private void startPcServerJoin(String enteredPin) {
        closeCurrentSession(false);
        host = false;
        pcServerMode = true;
        pcHelloSent = false;
        retryEnabled = true;
        pin = enteredPin;
        accepted = false;
        directConnected = false;
        nativeConnected = false;
        sessionKey = null;

        SharedPreferences prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        pcClientId = prefs.getString("pc_client_id", null);
        if (pcClientId == null || pcClientId.length() < 8) {
            pcClientId = UUID.randomUUID().toString().replace("-", "");
            prefs.edit().putString("pc_client_id", pcClientId).apply();
        }
        prefs.edit().putString(PREF_ROLE, "pc").putString(PREF_PIN, pin).apply();

        connectMqtt(() -> {
            subscribe(pcRoomTopic());
            showStatus("Connecting to PC server", "Looking for server PIN " + pin + "…");
        });
    }

    private void startHost(String existingPin, String existingSession) {
        closeCurrentSession(false);
        host = true;
        retryEnabled = true;
        pin = existingPin != null && existingPin.matches("\\d{4}") ? existingPin : String.format("%04d", random.nextInt(10000));
        sessionId = existingSession != null ? existingSession : randomHex(16);
        salt = randomHex(16);
        accepted = false;
        joinSent = false;
        directConnected = false;
        nativeConnected = false;

        try {
            ecdhKeyPair = createEcdhKeyPair();
            pinKey = derivePinKey(pin, hexToBytes(salt));
            hostPublicKey = Base64.encodeToString(ecdhKeyPair.getPublic().getEncoded(), Base64.NO_WRAP);
            SharedPreferences prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
            prefs.edit().putString(PREF_ROLE, "host").putString(PREF_PIN, pin)
                .putString(PREF_SESSION, sessionId).apply();

            connectMqtt(() -> {
                subscribe(toHostTopic(), relayHostTopic());
                publishRoomAdvertisement();
                showStatus("Your co-op PIN", pin + "\nWaiting for the other phone…");
            });
        } catch (Exception e) {
            showMessage("Could not start co-op: " + e.getMessage());
        }
    }

    private void startJoin(String enteredPin) {
        closeCurrentSession(false);
        host = false;
        retryEnabled = true;
        pin = enteredPin;
        accepted = false;
        joinSent = false;
        directConnected = false;
        nativeConnected = false;

        SharedPreferences prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        prefs.edit().putString(PREF_ROLE, "join").putString(PREF_PIN, pin).apply();

        connectMqtt(() -> {
            subscribe(roomTopic());
            showStatus("Joining co-op", "Looking for the phone hosting PIN " + pin + "…");
        });
    }

    private void connectMqtt(Runnable onReady) {
        try {
            try { if (mqtt != null) mqtt.close(); } catch (Exception ignored) {}
            String clientId = "hns-" + UUID.randomUUID().toString().replace("-", "");
            mqtt = new MqttAsyncClient(BROKER, clientId, new MemoryPersistence());
            mqtt.setCallback(new MqttCallbackExtended() {
                @Override public void connectComplete(boolean reconnect, String serverURI) {
                    mqttConnected = true;
                    if (pcServerMode) {
                        subscribe(pcRoomTopic());
                        pcHelloSent = false;
                        handler.removeCallbacks(pcHeartbeat);
                        handler.removeCallbacks(pcServerWatchdog);
                        handler.postDelayed(pcServerWatchdog, 10000);
                    } else if (host) {
                        subscribe(toHostTopic(), relayHostTopic());
                        publishRoomAdvertisement();
                        handler.removeCallbacks(roomHeartbeat);
                        handler.postDelayed(roomHeartbeat, 10000);
                    } else {
                        subscribe(roomTopic());
                    }
                    updateNativeConnection();
                }

                @Override public void connectionLost(Throwable cause) {
                    mqttConnected = false;
                    if (!directConnected) {
                        accepted = false;
                        joinSent = false;
                        sessionKey = null;
                    }
                    if (pcServerMode) {
                        accepted = false;
                        pcHelloSent = false;
                        sessionKey = null;
                        handler.removeCallbacks(pcHeartbeat);
                    }
                    updateNativeConnection();
                    showStatus("Reconnecting", "Restoring the co-op connection…");
                }

                @Override public void messageArrived(String topic, MqttMessage message) {
                    handleMqttMessage(topic, message.getPayload());
                }

                @Override public void deliveryComplete(IMqttDeliveryToken token) {}
            });

            MqttConnectOptions options = new MqttConnectOptions();
            options.setAutomaticReconnect(true);
            options.setCleanSession(true);
            options.setKeepAliveInterval(25);
            options.setConnectionTimeout(15);
            mqtt.connect(options, null, new IMqttActionListener() {
                @Override public void onSuccess(IMqttToken token) {
                    mqttConnected = true;
                    if (onReady != null)
                        onReady.run();
                }

                @Override public void onFailure(IMqttToken token, Throwable exception) {
                    mqttConnected = false;
                    showStatus("Connection service unavailable", "Retrying the co-op rendezvous…");
                    if (retryEnabled)
                        handler.postDelayed(() -> {
                            if (retryEnabled && (mqtt == null || !mqtt.isConnected()))
                                connectMqtt(onReady);
                        }, 5000);
                }
            });
        } catch (Exception e) {
            showMessage("Could not connect: " + e.getMessage());
        }
    }

    private void publishRoomAdvertisement() {
        if (!host || !mqttConnected || pinKey == null || ecdhKeyPair == null)
            return;
        try {
            JSONObject advert = new JSONObject();
            advert.put("type", "room");
            advert.put("session", sessionId);
            advert.put("salt", salt);
            advert.put("pub", hostPublicKey);
            advert.put("proof", hmacText(pinKey, sessionId + "|" + hostPublicKey));
            advert.put("timestamp", System.currentTimeMillis());
            publish(roomTopic(), advert.toString().getBytes(StandardCharsets.UTF_8), 1, true);
            handler.removeCallbacks(roomHeartbeat);
            handler.postDelayed(roomHeartbeat, 10000);
        } catch (Exception ignored) {}
    }

    private void publishJoinMessage() {
        if (host || !mqttConnected || sessionId == null || ecdhKeyPair == null || pinKey == null)
            return;
        try {
            JSONObject join = new JSONObject();
            String joinPublicKey = Base64.encodeToString(ecdhKeyPair.getPublic().getEncoded(), Base64.NO_WRAP);
            join.put("type", "join");
            join.put("session", sessionId);
            join.put("pub", joinPublicKey);
            join.put("proof", hmacText(pinKey, sessionId + "|" + hostPublicKey + "|" + joinPublicKey));
            publish(toHostTopic(), join.toString().getBytes(StandardCharsets.UTF_8), 1, false);
        } catch (Exception e) {
            showMessage("Could not join room: " + e.getMessage());
        }
    }

    private void handleMqttMessage(String topic, byte[] payload) {
        try {
            if (pcServerMode) {
                if (topic.equals(pcRoomTopic())) {
                    handlePcRoomAdvertisement(new JSONObject(new String(payload, StandardCharsets.UTF_8)));
                } else if (topic.equals(pcDownControlTopic())) {
                    handlePcServerControl(new JSONObject(new String(payload, StandardCharsets.UTF_8)));
                } else if (topic.equals(pcDownFrameTopic())) {
                    handleEncryptedFrame(payload);
                }
                return;
            }
            if (!host && topic.equals(roomTopic())) {
                handleRoomAdvertisement(new JSONObject(new String(payload, StandardCharsets.UTF_8)));
                return;
            }
            if (host && topic.equals(toHostTopic())) {
                JSONObject incoming = new JSONObject(new String(payload, StandardCharsets.UTF_8));
                if ("join".equals(incoming.optString("type")))
                    handleJoinMessage(incoming);
                else
                    handleSignal(incoming);
                return;
            }
            if (!host && topic.equals(toJoinTopic())) {
                handleSignal(new JSONObject(new String(payload, StandardCharsets.UTF_8)));
                return;
            }
            if (topic.equals(host ? relayHostTopic() : relayJoinTopic())) {
                handleEncryptedFrame(payload);
            }
        } catch (Exception ignored) {}
    }

    private void handlePcRoomAdvertisement(JSONObject advert) throws Exception {
        if (!pcServerMode || !"pc-room".equals(advert.optString("type")))
            return;
        long timestamp = advert.optLong("timestamp", 0);
        if (timestamp == 0 || System.currentTimeMillis() - timestamp > 30000)
            return;

        lastPcServerHeartbeatAt = System.currentTimeMillis();
        String incomingSession = advert.getString("session");
        if (incomingSession.equals(sessionId) && pcHelloSent)
            return;

        sessionId = incomingSession;
        salt = advert.getString("salt");
        pinKey = derivePinKey(pin, hexToBytes(salt));
        sessionKey = null;
        accepted = false;
        updateNativeConnection();
        sendCounter.set(1);
        receiveCounters[0] = receiveCounters[1] = 0;
        ecdhKeyPair = createEcdhKeyPair();
        subscribe(pcDownControlTopic(), pcDownFrameTopic());
        sendPcHello();
        showStatus("PC server found", "Authenticating with the server and waiting for the other player…");
    }

    private void sendPcHello() {
        if (!pcServerMode || !mqttConnected || sessionId == null || pinKey == null || ecdhKeyPair == null)
            return;
        try {
            String publicKey = Base64.encodeToString(ecdhKeyPair.getPublic().getEncoded(), Base64.NO_WRAP);
            JSONObject hello = new JSONObject();
            hello.put("type", "hello");
            hello.put("pub", publicKey);
            hello.put("proof", hmacText(pinKey, sessionId + "|" + pcClientId + "|" + publicKey));
            publish(pcUpHelloTopic(), hello.toString().getBytes(StandardCharsets.UTF_8), 0, false);
            pcHelloSent = true;
            handler.removeCallbacks(pcHeartbeat);
            handler.postDelayed(pcHeartbeat, 10000);
        } catch (Exception e) {
            showStatus("PC server connection error", e.getMessage());
        }
    }

    private void handlePcServerControl(JSONObject envelope) throws Exception {
        if (!pcServerMode || !sessionId.equals(envelope.optString("session")))
            return;
        String type = envelope.optString("type");
        JSONObject data = envelope.optJSONObject("data");
        if ("server-heartbeat".equals(type)) {
            lastPcServerHeartbeatAt = System.currentTimeMillis();
        } else if ("waiting".equals(type)) {
            showStatus("Waiting for Petra", "The PC server is ready. Waiting for the second phone…");
        } else if ("peer".equals(type) && data != null) {
            String peerId = data.getString("client");
            String peerPublicKey = data.getString("pub");
            String proof = data.getString("proof");
            String expected = hmacText(pinKey, sessionId + "|" + peerId + "|" + peerPublicKey);
            if (!constantTimeEquals(expected, proof))
                return;
            byte[] shared = deriveSharedSecret(ecdhKeyPair, peerPublicKey);
            sessionKey = deriveSessionKey(shared, pinKey, sessionId);
            host = data.optInt("role", 0) == 0;
            sendCounter.set(1);
            receiveCounters[0] = receiveCounters[1] = 0;
            accepted = true;
            updateNativeConnection();
            showStatus("Co-op connected", "Both phones are connected through your PC server.");
        } else if ("peer-disconnected".equals(type)) {
            accepted = false;
            sessionKey = null;
            updateNativeConnection();
            showStatus("Player disconnected", "Holding the session while the other phone reconnects…");
        } else if ("full".equals(type) || "rejected".equals(type)) {
            showStatus("PC server refused connection", data != null ? data.optString("reason", "Unable to join this room.") : "Unable to join this room.");
        }
    }

    private void handleRoomAdvertisement(JSONObject advert) throws Exception {
        if (!"room".equals(advert.optString("type")))
            return;
        long timestamp = advert.optLong("timestamp", 0);
        if (timestamp == 0 || System.currentTimeMillis() - timestamp > 30000)
            return;
        String incomingSession = advert.getString("session");
        String incomingSalt = advert.getString("salt");
        String incomingPublicKey = advert.getString("pub");
        byte[] derivedPinKey = derivePinKey(pin, hexToBytes(incomingSalt));
        String expected = hmacText(derivedPinKey, incomingSession + "|" + incomingPublicKey);
        if (!constantTimeEquals(expected, advert.getString("proof"))) {
            showStatus("PIN not accepted", "The room could not be authenticated. Check the four digits.");
            return;
        }

        if (joinSent && incomingSession.equals(sessionId) && incomingPublicKey.equals(hostPublicKey))
            return;
        resetPeerConnection();
        accepted = false;
        sessionKey = null;
        sessionId = incomingSession;
        salt = incomingSalt;
        hostPublicKey = incomingPublicKey;
        pinKey = derivedPinKey;
        ecdhKeyPair = createEcdhKeyPair();
        subscribe(toJoinTopic(), relayJoinTopic());
        publishJoinMessage();
        joinSent = true;
        showStatus("Pairing", "Authenticating the other phone…");
    }

    private void handleJoinMessage(JSONObject join) throws Exception {
        if (!"join".equals(join.optString("type")) || !sessionId.equals(join.optString("session")) || (accepted && directConnected))
            return;

        resetPeerConnection();
        accepted = false;
        String joinPublicKey = join.getString("pub");
        String expected = hmacText(pinKey, sessionId + "|" + hostPublicKey + "|" + joinPublicKey);
        if (!constantTimeEquals(expected, join.getString("proof")))
            return;

        byte[] shared = deriveSharedSecret(ecdhKeyPair, joinPublicKey);
        sessionKey = deriveSessionKey(shared, pinKey, sessionId);
        accepted = true;
        String acceptProof = hmacText(pinKey, sessionId + "|accept|" + joinPublicKey);
        JSONObject accept = new JSONObject();
        accept.put("type", "accept");
        accept.put("session", sessionId);
        accept.put("proof", acceptProof);
        publish(toJoinTopic(), accept.toString().getBytes(StandardCharsets.UTF_8), 1, false);

        updateNativeConnection();
        showStatus("Paired", "Connected. Establishing the direct phone-to-phone route…");
        createPeerConnection(true);
    }

    private void handleSignal(JSONObject signal) throws Exception {
        if (!sessionId.equals(signal.optString("session")))
            return;
        String type = signal.optString("type");
        if ("accept".equals(type) && !host) {
            String expected = hmacText(pinKey, sessionId + "|accept|" + Base64.encodeToString(ecdhKeyPair.getPublic().getEncoded(), Base64.NO_WRAP));
            if (!constantTimeEquals(expected, signal.getString("proof")))
                return;
            byte[] shared = deriveSharedSecret(ecdhKeyPair, hostPublicKey);
            sessionKey = deriveSessionKey(shared, pinKey, sessionId);
            accepted = true;
            updateNativeConnection();
            showStatus("Paired", "Connected. Establishing the direct phone-to-phone route…");
            createPeerConnection(false);
            return;
        }

        String value = signal.optString("value");
        if (sessionKey == null || !verifySignal(type, value, signal.optString("mac")))
            return;

        if ("offer".equals(type) && !host) {
            SessionDescription offer = new SessionDescription(SessionDescription.Type.OFFER, value);
            peerConnection.setRemoteDescription(new SimpleSdpObserver() {
                @Override public void onSetSuccess() {
                    flushPendingCandidates();
                    createAnswer();
                }
            }, offer);
        } else if ("answer".equals(type) && host) {
            SessionDescription answer = new SessionDescription(SessionDescription.Type.ANSWER, value);
            peerConnection.setRemoteDescription(new SimpleSdpObserver() {
                @Override public void onSetSuccess() {
                    flushPendingCandidates();
                }
            }, answer);
        } else if ("ice".equals(type)) {
            JSONObject candidate = new JSONObject(value);
            IceCandidate ice = new IceCandidate(candidate.getString("mid"), candidate.getInt("line"), candidate.getString("sdp"));
            if (peerConnection != null && peerConnection.getRemoteDescription() != null)
                peerConnection.addIceCandidate(ice);
            else
                pendingCandidates.add(ice);
        }
    }

    private void createPeerConnection(boolean offerer) {
        try {
            initializeWebRtc();
            List<PeerConnection.IceServer> iceServers = Arrays.asList(
                PeerConnection.IceServer.builder("stun:stun.l.google.com:19302").createIceServer(),
                PeerConnection.IceServer.builder("stun:stun1.l.google.com:19302").createIceServer(),
                PeerConnection.IceServer.builder("stun:stun2.l.google.com:19302").createIceServer()
            );
            PeerConnection.RTCConfiguration config = new PeerConnection.RTCConfiguration(iceServers);
            config.sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN;
            peerConnection = factory.createPeerConnection(config, new PeerConnection.Observer() {
                @Override public void onSignalingChange(PeerConnection.SignalingState state) {}
                @Override public void onIceConnectionChange(PeerConnection.IceConnectionState state) {
                    boolean iceReady = state == PeerConnection.IceConnectionState.CONNECTED
                        || state == PeerConnection.IceConnectionState.COMPLETED;
                    directConnected = iceReady && dataChannel != null && dataChannel.state() == DataChannel.State.OPEN;
                    updateNativeConnection();
                    if (directConnected)
                        showStatus("Connected directly", "Phone-to-phone connection is active.");
                    else if (accepted)
                        showStatus("Connected by relay", "Using the automatic relay route.");
                }
                @Override public void onIceConnectionReceivingChange(boolean receiving) {}
                @Override public void onIceGatheringChange(PeerConnection.IceGatheringState state) {}
                @Override public void onIceCandidate(IceCandidate candidate) {
                    try {
                        JSONObject value = new JSONObject();
                        value.put("mid", candidate.sdpMid);
                        value.put("line", candidate.sdpMLineIndex);
                        value.put("sdp", candidate.sdp);
                        sendSignal("ice", value.toString());
                    } catch (Exception ignored) {}
                }
                @Override public void onIceCandidatesRemoved(IceCandidate[] candidates) {}
                @Override public void onAddStream(org.webrtc.MediaStream stream) {}
                @Override public void onRemoveStream(org.webrtc.MediaStream stream) {}
                @Override public void onDataChannel(DataChannel channel) {
                    dataChannel = channel;
                    registerDataChannel(channel);
                }
                @Override public void onRenegotiationNeeded() {}
                @Override public void onAddTrack(org.webrtc.RtpReceiver receiver, org.webrtc.MediaStream[] streams) {}
                public void onConnectionChange(PeerConnection.PeerConnectionState state) {}
                public void onTrack(org.webrtc.RtpTransceiver transceiver) {}
                public void onRemoveTrack(org.webrtc.RtpReceiver receiver) {}
            });

            if (peerConnection == null)
                throw new IllegalStateException("WebRTC could not create a peer connection");

            if (offerer) {
                DataChannel.Init init = new DataChannel.Init();
                init.ordered = true;
                dataChannel = peerConnection.createDataChannel("hns-gba-link", init);
                registerDataChannel(dataChannel);
                peerConnection.createOffer(new SimpleSdpObserver() {
                    @Override public void onCreateSuccess(SessionDescription description) {
                        peerConnection.setLocalDescription(new SimpleSdpObserver() {
                            @Override public void onSetSuccess() {
                                sendSignal("offer", description.description);
                            }
                        }, description);
                    }
                }, new MediaConstraints());
            }
        } catch (Exception e) {
            directConnected = false;
            showStatus("Connected by relay", "Direct route unavailable; using the relay.");
        }
    }

    private void createAnswer() {
        if (peerConnection == null)
            return;
        peerConnection.createAnswer(new SimpleSdpObserver() {
            @Override public void onCreateSuccess(SessionDescription description) {
                peerConnection.setLocalDescription(new SimpleSdpObserver() {
                    @Override public void onSetSuccess() {
                        sendSignal("answer", description.description);
                    }
                }, description);
            }
        }, new MediaConstraints());
    }

    private void registerDataChannel(DataChannel channel) {
        if (channel == null)
            return;
        channel.registerObserver(new DataChannel.Observer() {
            @Override public void onBufferedAmountChange(long previousAmount) {}
            @Override public void onStateChange() {
                if (channel.state() == DataChannel.State.OPEN) {
                    directConnected = true;
                    showStatus("Connected directly", "Phone-to-phone connection is active.");
                } else if (accepted && !directConnected) {
                    showStatus("Connected by relay", "Using the automatic relay route.");
                }
                updateNativeConnection();
            }
            @Override public void onMessage(DataChannel.Buffer buffer) {
                byte[] encrypted = new byte[buffer.data.remaining()];
                buffer.data.get(encrypted);
                handleEncryptedFrame(encrypted);
            }
        });
    }

    private void flushPendingCandidates() {
        if (peerConnection == null || peerConnection.getRemoteDescription() == null)
            return;
        for (IceCandidate candidate : pendingCandidates)
            peerConnection.addIceCandidate(candidate);
        pendingCandidates.clear();
    }

    private void sendSignal(String type, String value) {
        try {
            JSONObject message = new JSONObject();
            message.put("type", type);
            message.put("session", sessionId);
            message.put("value", value);
            message.put("mac", hmacText(sessionKey, sessionId + "|" + type + "|" + value));
            publish(host ? toJoinTopic() : toHostTopic(), message.toString().getBytes(StandardCharsets.UTF_8), 1, false);
        } catch (Exception ignored) {}
    }

    private boolean verifySignal(String type, String value, String mac) {
        return constantTimeEquals(hmacText(sessionKey, sessionId + "|" + type + "|" + value), mac);
    }

    public static boolean sendFrame(byte[] frame) {
        PeerLinkManager manager = instance;
        return manager != null && manager.sendFrameInternal(frame);
    }

    private boolean sendFrameInternal(byte[] frame) {
        if (!accepted || sessionKey == null || frame == null)
            return false;
        try {
            byte[] encrypted = encryptFrame(frame);
            if (pcServerMode) {
                if (mqttConnected) {
                    publish(pcUpFrameTopic(), encrypted, 0, false);
                    return true;
                }
                return false;
            }
            if (directConnected && dataChannel != null && dataChannel.state() == DataChannel.State.OPEN) {
                if (dataChannel.send(new DataChannel.Buffer(ByteBuffer.wrap(encrypted), true)))
                    return true;
            }
            if (mqttConnected) {
                publish(host ? relayJoinTopic() : relayHostTopic(), encrypted, 1, false);
                return true;
            }
        } catch (Exception ignored) {}
        return false;
    }

    private byte[] encryptFrame(byte[] frame) throws Exception {
        long counter = sendCounter.getAndIncrement();
        ByteBuffer nonce = ByteBuffer.allocate(12);
        nonce.putInt(host ? 0 : 1);
        nonce.putLong(counter);
        byte[] nonceBytes = nonce.array();
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(sessionKey, "AES"), new GCMParameterSpec(128, nonceBytes));
        byte[] encrypted = cipher.doFinal(frame);
        ByteBuffer output = ByteBuffer.allocate(nonceBytes.length + encrypted.length);
        output.put(nonceBytes).put(encrypted);
        return output.array();
    }

    private void handleEncryptedFrame(byte[] payload) {
        if (sessionKey == null || payload == null || payload.length < 28)
            return;
        try {
            ByteBuffer input = ByteBuffer.wrap(payload);
            int sender = input.getInt();
            long counter = input.getLong();
            if (sender < 0 || sender > 1 || counter <= receiveCounters[sender])
                return;
            byte[] nonce = Arrays.copyOfRange(payload, 0, 12);
            byte[] encrypted = Arrays.copyOfRange(payload, 12, payload.length);
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, new SecretKeySpec(sessionKey, "AES"), new GCMParameterSpec(128, nonce));
            byte[] frame = cipher.doFinal(encrypted);
            receiveCounters[sender] = counter;
            nativePeerFrame(frame);
        } catch (Exception ignored) {}
    }

    private void updateNativeConnection() {
        boolean connected = accepted && (mqttConnected || directConnected);
        if (connected != nativeConnected) {
            nativeConnected = connected;
            if (connected)
                nativePeerConnected(host);
            else
                nativePeerDisconnected();
        }
    }

    private void subscribe(String... topics) {
        if (mqtt == null || !mqtt.isConnected())
            return;
        for (String topic : topics) {
            try { mqtt.subscribe(topic, 1); } catch (Exception ignored) {}
        }
    }

    private void publish(String topic, byte[] payload, int qos, boolean retained) {
        if (mqtt == null || !mqtt.isConnected())
            return;
        try {
            MqttMessage message = new MqttMessage(payload);
            message.setQos(qos);
            message.setRetained(retained);
            mqtt.publish(topic, message);
        } catch (Exception ignored) {}
    }

    private String pcRootTopic() { return "hns/johto-pc/" + pin + "/" + sessionId; }
    private String pcRoomTopic() { return "hns/johto-pc/" + pin + "/room"; }
    private String pcUpHelloTopic() { return pcRootTopic() + "/up/" + pcClientId + "/hello"; }
    private String pcUpHeartbeatTopic() { return pcRootTopic() + "/up/" + pcClientId + "/heartbeat"; }
    private String pcUpFrameTopic() { return pcRootTopic() + "/up/" + pcClientId + "/frame"; }
    private String pcUpLeaveTopic() { return pcRootTopic() + "/up/" + pcClientId + "/leave"; }
    private String pcDownControlTopic() { return pcRootTopic() + "/down/" + pcClientId + "/control"; }
    private String pcDownFrameTopic() { return pcRootTopic() + "/down/" + pcClientId + "/frame"; }

    private String roomTopic() { return "hns/johto/" + pin + "/room"; }
    private String toHostTopic() { return "hns/johto/" + pin + "/" + sessionId + "/to-host"; }
    private String toJoinTopic() { return "hns/johto/" + pin + "/" + sessionId + "/to-join"; }
    private String relayHostTopic() { return "hns/johto/" + pin + "/" + sessionId + "/relay/host"; }
    private String relayJoinTopic() { return "hns/johto/" + pin + "/" + sessionId + "/relay/join"; }

    private void initializeWebRtc() {
        if (!webRtcInitialized) {
            PeerConnectionFactory.initialize(
                PeerConnectionFactory.InitializationOptions.builder(activity).createInitializationOptions());
            webRtcInitialized = true;
        }
        if (factory == null)
            factory = PeerConnectionFactory.builder().createPeerConnectionFactory();
    }

    private KeyPair createEcdhKeyPair() throws Exception {
        KeyPairGenerator generator = KeyPairGenerator.getInstance("EC");
        generator.initialize(new ECGenParameterSpec("secp256r1"));
        return generator.generateKeyPair();
    }

    private byte[] derivePinKey(String value, byte[] saltBytes) throws Exception {
        PBEKeySpec spec = new PBEKeySpec(value.toCharArray(), saltBytes, PBKDF2_ITERATIONS, 256);
        return SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).getEncoded();
    }

    private byte[] deriveSharedSecret(KeyPair local, String remotePublicKey) throws Exception {
        byte[] encoded = Base64.decode(remotePublicKey, Base64.DEFAULT);
        PublicKey remote = KeyFactory.getInstance("EC").generatePublic(new X509EncodedKeySpec(encoded));
        KeyAgreement agreement = KeyAgreement.getInstance("ECDH");
        agreement.init(local.getPrivate());
        agreement.doPhase(remote, true);
        return agreement.generateSecret();
    }

    private byte[] deriveSessionKey(byte[] shared, byte[] pinSecret, String session) throws Exception {
        byte[] input = new byte[shared.length + pinSecret.length];
        System.arraycopy(shared, 0, input, 0, shared.length);
        System.arraycopy(pinSecret, 0, input, shared.length, pinSecret.length);
        byte[] prk = hmac(hexToBytes(session), input);
        return hmac(prk, "HNS-JOHTO-COOP".getBytes(StandardCharsets.UTF_8));
    }

    private String hmacText(byte[] key, String value) throws Exception {
        return Base64.encodeToString(hmac(key, value.getBytes(StandardCharsets.UTF_8)), Base64.NO_WRAP);
    }

    private byte[] hmac(byte[] key, byte[] value) throws Exception {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        return mac.doFinal(value);
    }

    private String randomHex(int bytes) {
        byte[] value = new byte[bytes];
        random.nextBytes(value);
        StringBuilder out = new StringBuilder(bytes * 2);
        for (byte b : value)
            out.append(String.format("%02x", b & 0xFF));
        return out.toString();
    }

    private byte[] hexToBytes(String value) {
        byte[] out = new byte[value.length() / 2];
        for (int i = 0; i < out.length; i++)
            out[i] = (byte) Integer.parseInt(value.substring(i * 2, i * 2 + 2), 16);
        return out;
    }

    private boolean constantTimeEquals(String a, String b) {
        if (a == null || b == null)
            return false;
        return MessageDigest.isEqual(a.getBytes(StandardCharsets.UTF_8), b.getBytes(StandardCharsets.UTF_8));
    }

    private void resetPeerConnection() {
        try { if (peerConnection != null) peerConnection.close(); } catch (Exception ignored) {}
        try { if (dataChannel != null) dataChannel.close(); } catch (Exception ignored) {}
        peerConnection = null;
        dataChannel = null;
        directConnected = false;
        pendingCandidates.clear();
        sendCounter.set(1);
        receiveCounters[0] = receiveCounters[1] = 0;
    }

    private void closeCurrentSession(boolean clearPreferences) {
        boolean wasConnected = nativeConnected;
        retryEnabled = false;
        handler.removeCallbacks(roomHeartbeat);
        handler.removeCallbacks(pcHeartbeat);
        handler.removeCallbacks(pcServerWatchdog);
        if (pcServerMode && mqtt != null && mqtt.isConnected() && pcClientId != null)
            publish(pcUpLeaveTopic(), new byte[0], 0, false);
        if (clearPreferences && host && mqtt != null && mqtt.isConnected() && pin != null && sessionId != null)
            publish(roomTopic(), new byte[0], 1, true);
        try { if (peerConnection != null) peerConnection.close(); } catch (Exception ignored) {}
        try { if (dataChannel != null) dataChannel.close(); } catch (Exception ignored) {}
        try { if (mqtt != null && mqtt.isConnected()) mqtt.disconnect(); } catch (Exception ignored) {}
        peerConnection = null;
        dataChannel = null;
        mqtt = null;
        mqttConnected = false;
        directConnected = false;
        pcServerMode = false;
        pcHelloSent = false;
        accepted = false;
        nativeConnected = false;
        sessionKey = null;
        pinKey = null;
        ecdhKeyPair = null;
        pendingCandidates.clear();
        sendCounter.set(1);
        receiveCounters[0] = receiveCounters[1] = 0;
        if (clearPreferences && activity != null)
            activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().clear().apply();
        if (wasConnected)
            nativePeerDisconnected();
    }

    private void showStatus(String title, String message) {
        Activity current = activity;
        if (current == null)
            return;
        current.runOnUiThread(() -> {
            if (statusDialog != null && statusDialog.isShowing()) {
                statusDialog.setTitle(title);
                statusDialog.setMessage(message);
            } else {
                statusDialog = new AlertDialog.Builder(current)
                    .setTitle(title)
                    .setMessage(message)
                    .setPositiveButton("OK", (dialog, which) -> dialog.dismiss())
                    .setNegativeButton("Disconnect", (dialog, which) -> closeCurrentSession(true))
                    .create();
                statusDialog.show();
            }
        });
    }

    private void showMessage(String message) {
        Activity current = activity;
        if (current != null)
            current.runOnUiThread(() -> new AlertDialog.Builder(current).setMessage(message).setPositiveButton("OK", null).show());
    }

    private static final class SimpleSdpObserver implements SdpObserver {
        @Override public void onCreateSuccess(SessionDescription description) {}
        @Override public void onSetSuccess() {}
        @Override public void onCreateFailure(String error) {}
        @Override public void onSetFailure(String error) {}
    }

    private static native void nativeInitialize();
    private native void nativePeerConnected(boolean hostRole);
    private native void nativePeerDisconnected();
    private native void nativePeerFrame(byte[] frame);
}
