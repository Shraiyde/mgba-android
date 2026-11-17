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
import android.widget.Toast;

import org.eclipse.paho.client.mqttv3.IMqttActionListener;
import org.eclipse.paho.client.mqttv3.IMqttDeliveryToken;
import org.eclipse.paho.client.mqttv3.IMqttToken;
import org.eclipse.paho.client.mqttv3.MqttAsyncClient;
import org.eclipse.paho.client.mqttv3.MqttCallbackExtended;
import org.eclipse.paho.client.mqttv3.MqttConnectOptions;
import org.eclipse.paho.client.mqttv3.MqttMessage;
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence;
import org.json.JSONObject;
import org.webrtc.CandidatePairChangeEvent;
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
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

import javax.crypto.Cipher;
import javax.crypto.KeyAgreement;
import javax.crypto.Mac;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * Co-op link. Primary path: one-tap Join PC Host over ZeroTier TCP to the
 * baked-in World Host. No MQTT / WebRTC / TURN for game data.
 */
public final class PeerLinkManager {
    private static final String BROKER = "wss://broker.emqx.io:8084/mqtt";
    private static final String PREFS = "hns_peer_link";
    private static final String PREF_ROLE = "role";
    private static final String PREF_PIN = "pin";
    private static final String PREF_SESSION = "session";
    private static final String PREF_PC_HOST_ADDR = "pc_host_addr";
    private static final String PREF_TURN_URI = "turn_uri";
    private static final String PREF_TURN_USER = "turn_user";
    private static final String PREF_TURN_PASS = "turn_pass";
    private static final String PREF_DIAG_LOGS = "diag_logs";
    private static final int PIN_DIGITS = 6;
    private static final String PIN_PATTERN = "\\d{6}";
    private static final int PBKDF2_ITERATIONS = 500000;
    private static final int MAX_PEER_FRAME_SIZE = 16 + 4096;
    private static final int MAX_ENCRYPTED_FRAME_SIZE = 12 + MAX_PEER_FRAME_SIZE + 16;
    private static final int MAX_MQTT_PAYLOAD_SIZE = 16 * 1024;
    private static final int DIRECT_TIMEOUT_MS = 15000;
    private static final int PC_DROP_TOLERANCE_MS = 10000;

    private static volatile Activity activity;
    private static volatile Context appContext;
    private static volatile PeerLinkManager instance;
    private static volatile boolean webRtcInitialized;
    // Native emulation/render threads must never wait on MQTT/WebRTC work.
    // POSITION uses AbortPolicy (not DiscardOldest): dropped intermediates made
    // the peer avatar skip tiles. A deeper queue keeps ordered tile updates;
    // sendFrame returns false when full so native retries the same coords.
    // Mailbox / SIO likewise use AbortPolicy.
    private static final int PEER_HDR_TYPE_OFFSET = 6;
    private static final int MPEER_MSG_POSITION = 16;
    private static final int MPEER_MSG_SIO_MULTI = 23;
    private static final ThreadPoolExecutor positionSender = new ThreadPoolExecutor(
        1, 1, 0L, TimeUnit.MILLISECONDS, new ArrayBlockingQueue<>(32),
        task -> {
            Thread thread = new Thread(task, "HNS-Peer-Pos");
            thread.setDaemon(true);
            return thread;
        }, new ThreadPoolExecutor.AbortPolicy());
    private static final ThreadPoolExecutor mailboxSender = new ThreadPoolExecutor(
        1, 1, 0L, TimeUnit.MILLISECONDS, new ArrayBlockingQueue<>(64),
        task -> {
            Thread thread = new Thread(task, "HNS-Peer-Mailbox");
            thread.setDaemon(true);
            return thread;
        }, new ThreadPoolExecutor.AbortPolicy());
    private static final ThreadPoolExecutor sioFrameSender = new ThreadPoolExecutor(
        1, 1, 0L, TimeUnit.MILLISECONDS, new ArrayBlockingQueue<>(64),
        task -> {
            Thread thread = new Thread(task, "HNS-Peer-SIO");
            thread.setDaemon(true);
            return thread;
        }, new ThreadPoolExecutor.AbortPolicy());
    private static final ExecutorService cryptoExecutor = Executors.newSingleThreadExecutor(task -> {
        Thread thread = new Thread(task, "HNS-Peer-Crypto");
        thread.setDaemon(true);
        return thread;
    });

    private final SecureRandom random = new SecureRandom();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable roomHeartbeat = new Runnable() {
        @Override public void run() {
            if (host && mqttConnected) {
                // publishRoomAdvertisement() owns the single heartbeat schedule.
                publishRoomAdvertisement();
            }
        }
    };
    private final Runnable pcHeartbeat = new Runnable() {
        @Override public void run() {
            // Legacy MQTT PC heartbeat removed — ZeroTier TCP host beats instead.
        }
    };
    private final Runnable directTimeout = new Runnable() {
        @Override public void run() {
            if (pcServerMode)
                return; // ZeroTier TCP path — never WebRTC/TURN/MQTT relay statuses.
            if (accepted && !directConnected) {
                showStatus("Reconnecting…", "Waiting for the peer link…");
                handler.removeCallbacks(directRetry);
                handler.postDelayed(directRetry, 8000);
            }
        }
    };
    private final Runnable directRetry = new Runnable() {
        @Override public void run() {
            if (pcServerMode || !accepted || sessionKey == null || directConnected)
                return;
            try { if (peerConnection != null) peerConnection.close(); } catch (Exception ignored) {}
            try { if (dataChannel != null) dataChannel.close(); } catch (Exception ignored) {}
            peerConnection = null;
            dataChannel = null;
            directViaTurn = false;
            pendingCandidates.clear();
            beginDirectLink(host);
        }
    };
    private final Runnable pcServerWatchdog = new Runnable() {
        @Override public void run() {
            if (!pcServerMode || !retryEnabled) {
                return;
            }
            if (accepted && lastPcServerHeartbeatAt != 0
                && System.currentTimeMillis() - lastPcServerHeartbeatAt > PC_DROP_TOLERANCE_MS
                && !pcTcpConnected) {
                // Soft hold: keep sessionKey/accepted so partner stays visible;
                // only surface Reconnecting once while TCP retries.
                showStatus("Reconnecting…", "Waiting for the PC host…");
                ensurePcTcpLink();
            }
            handler.postDelayed(this, 2000);
        }
    };
    private final Runnable acceptRetry = new Runnable() {
        @Override public void run() {
            if (!host || !accepted || directConnected || !mqttConnected || acceptedJoinPublicKey == null || acceptRetryAttempts >= 3)
                return;
            ++acceptRetryAttempts;
            publishAcceptMessage();
            if (acceptRetryAttempts < 3)
                handler.postDelayed(this, 2500);
        }
    };
    private final AtomicLong sendCounter = new AtomicLong(1);
    /** Index 0/1 = phone roles; index 2 = PC World Host (protocol v3+). */
    private final long[] receiveCounters = new long[] {0, 0, 0};
    private final List<IceCandidate> pendingCandidates = Collections.synchronizedList(new ArrayList<>());
    private final PcHostSocket pcSocket = new PcHostSocket();
    private volatile String pcHostAddress;
    private volatile int pcHostPort;
    private volatile boolean pcTcpConnected;
    private volatile String lastStatusTitle;
    private volatile boolean pcReconnectToastShown;

    private synchronized void resetReceiveCounters() {
        receiveCounters[0] = receiveCounters[1] = receiveCounters[2] = 0;
    }

    private volatile MqttAsyncClient mqtt;
    private volatile PeerConnectionFactory factory;
    private volatile PeerConnection peerConnection;
    private volatile DataChannel dataChannel;
    private volatile String pin;
    private volatile String sessionId;
    private volatile String salt;
    private volatile String hostPublicKey;
    private volatile String acceptedJoinPublicKey;
    private volatile int acceptRetryAttempts;
    private volatile String toHostTopic;
    private volatile String toJoinTopic;
    private volatile String relayHostTopic;
    private volatile String relayJoinTopic;
    private volatile boolean host;
    private volatile boolean pcServerMode;
    private volatile boolean pcHelloSent;
    private volatile long lastPcServerHeartbeatAt;
    private volatile String pcClientId;
    private volatile String pcPeerId;
    private volatile String pcPeerPublicKey;
    private volatile boolean mqttConnected;
    private volatile boolean retryEnabled;
    private volatile boolean accepted;
    private volatile boolean joinSent;
    private volatile boolean directConnected;
    /** True when the selected ICE pair uses a TURN relay candidate. */
    private volatile boolean directViaTurn;
    private volatile boolean nativeConnected;
    private volatile KeyPair ecdhKeyPair;
    private volatile byte[] pinKey;
    private volatile byte[] sessionKey;

    private PeerLinkManager() {}

    public static synchronized void initialize(Activity value) {
        activity = value;
        if (value != null)
            appContext = value.getApplicationContext();
        boolean created = instance == null;
        if (created)
            instance = new PeerLinkManager();
        nativeInitialize();
        PeerLinkManager manager = instance;
        if (value != null) {
            SharedPreferences prefs = value.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
            /* Drop any previously saved custom TURN so the UI never resurrects it. */
            if (prefs.contains(PREF_TURN_URI) || prefs.contains(PREF_TURN_USER) || prefs.contains(PREF_TURN_PASS)) {
                prefs.edit()
                    .remove(PREF_TURN_URI)
                    .remove(PREF_TURN_USER)
                    .remove(PREF_TURN_PASS)
                    .apply();
            }
            // Diag / HnsCoop log hooks stay off — Stephen does not want debug logs.
            nativeSetDiagLogs(false);
            manager.pcSocket.setListener(new PcHostSocket.Listener() {
                @Override public void onReady(int role, int players) {
                    manager.host = role == 0;
                    if (manager.sessionKey == null)
                        return;
                    manager.sendCounter.set(1);
                    manager.resetReceiveCounters();
                    manager.accepted = true;
                    manager.lastPcServerHeartbeatAt = System.currentTimeMillis();
                    manager.pcReconnectToastShown = false;
                    manager.reopenNativeSession();
                    manager.showStatus("Connected to PC host", "Linked to the PC world host.");
                }
                @Override public void onConnected() {
                    manager.pcTcpConnected = true;
                    manager.lastPcServerHeartbeatAt = System.currentTimeMillis();
                    manager.pcReconnectToastShown = false;
                    manager.updateNativeConnection();
                    if (manager.accepted)
                        manager.showStatus("Connected to PC host", "Linked to the PC world host.");
                }
                @Override public void onDisconnected() {
                    manager.pcTcpConnected = false;
                    // Keep accepted/sessionKey so partner sprites stay up during soft resume.
                    manager.updateNativeConnection();
                    if (manager.pcServerMode && manager.accepted && manager.retryEnabled)
                        manager.showStatus("Reconnecting…", "Waiting for the PC host…");
                }
                @Override public void onFrame(byte[] encrypted) {
                    manager.lastPcServerHeartbeatAt = System.currentTimeMillis();
                    manager.handleEncryptedFrame(encrypted);
                }
                @Override public void onHostBusy() {
                    manager.showStatus("PC host full", "Two phones are already connected.");
                }
            });
        }
        if (created)
            manager.handler.post(manager::restoreSavedSession);
    }

    /** Force ROM/native story+presence re-sync after the Activity resumes. */
    public static void onActivityResumed() {
        PeerLinkManager manager = instance;
        if (manager != null && manager.accepted)
            nativeRequestStoryResync();
    }

    private void restoreSavedSession() {
        if (retryEnabled || activity == null)
            return;
        SharedPreferences prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String savedPin = prefs.getString(PREF_PIN, null);
        if ("pc".equals(prefs.getString(PREF_ROLE, null))
            && savedPin != null && savedPin.matches(PIN_PATTERN)) {
            String endpoint = prefs.getString(PREF_PC_HOST_ADDR, BuiltinPcHostConfig.defaultEndpoint());
            startPcHostJoin(savedPin, endpoint);
        }
    }

    public static void syncNativeConnection() {
        PeerLinkManager manager = instance;
        if (manager != null)
            manager.syncNativeConnectionInternal();
    }

    private void syncNativeConnectionInternal() {
        // PC mode: stay natively linked while the session is accepted so partner
        // visibility holds across a brief TCP resume (~10s).
        boolean connected = pcServerMode
            ? (accepted && sessionKey != null)
            : (accepted && (mqttConnected || directConnected));
        nativeConnected = connected;
        if (connected)
            nativePeerConnected(host);
        else
            nativePeerDisconnected();
    }

    public static synchronized void detachActivity(Activity value) {
        if (activity == value)
            activity = null;
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

        List<String> choices = new ArrayList<>();
        choices.add("Join PC Host");
        if ("pc".equals(savedRole) && savedPin != null && savedPin.matches(PIN_PATTERN))
            choices.add("Reconnect to PC Host");
        choices.add("Play Offline");

        new AlertDialog.Builder(activity)
            .setTitle("Heart & Soul Co-op")
            .setItems(choices.toArray(new String[0]), (dialog, which) -> {
                String choice = choices.get(which);
                if ("Join PC Host".equals(choice))
                    showPcHostJoinDialog();
                else if ("Reconnect to PC Host".equals(choice)) {
                    String endpoint = prefs.getString(PREF_PC_HOST_ADDR, BuiltinPcHostConfig.defaultEndpoint());
                    startPcHostJoin(savedPin, endpoint);
                } else if ("Play Offline".equals(choice)) {
                    closeCurrentSession(true);
                    showStatus("Offline mode", "The co-op session has been stopped.");
                }
            })
            .show();
    }

    private List<PeerConnection.IceServer> buildIceServers() {
        List<PeerConnection.IceServer> servers = new ArrayList<>();
        servers.add(PeerConnection.IceServer.builder("stun:stun.l.google.com:19302").createIceServer());
        servers.add(PeerConnection.IceServer.builder("stun:stun1.l.google.com:19302").createIceServer());
        servers.add(PeerConnection.IceServer.builder("stun:stun2.l.google.com:19302").createIceServer());
        /* Built-in Metered ICE: one STUN entry + one TURN IceServer covering all
         * turn/turns URLs with shared username/credential (WebRTC-preferred form). */
        List<String> stunUrls = new ArrayList<>();
        List<String> turnUrls = new ArrayList<>();
        for (String url : BuiltinTurnConfig.URLS) {
            if (url.startsWith("turn:") || url.startsWith("turns:"))
                turnUrls.add(url);
            else
                stunUrls.add(url);
        }
        if (!stunUrls.isEmpty())
            servers.add(PeerConnection.IceServer.builder(stunUrls).createIceServer());
        if (!turnUrls.isEmpty()) {
            servers.add(PeerConnection.IceServer.builder(turnUrls)
                .setUsername(BuiltinTurnConfig.username())
                .setPassword(BuiltinTurnConfig.credential())
                .createIceServer());
        }
        return servers;
    }

    private void announceDirectLink() {
        if (pcServerMode) {
            showStatus("Connected to PC host", "Linked to the PC world host.");
            return;
        }
        if (directViaTurn)
            showStatus("Connected to PC host", "Legacy path unused.");
        else
            showStatus("Connected to PC host", "Legacy path unused.");
    }

    private static boolean candidateLooksLikeRelay(IceCandidate candidate) {
        if (candidate == null || candidate.sdp == null)
            return false;
        String sdp = candidate.sdp;
        return sdp.contains(" typ relay ") || sdp.endsWith(" typ relay")
            || sdp.contains(" typ relay\r") || sdp.contains(" typ relay\n");
    }

    private void refreshDirectViaTurnFromPair(IceCandidate local, IceCandidate remote) {
        directViaTurn = candidateLooksLikeRelay(local) || candidateLooksLikeRelay(remote);
    }

    private void showJoinPinDialog() {
        SharedPreferences prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String savedRole = prefs.getString(PREF_ROLE, null);
        String savedPin = prefs.getString(PREF_PIN, null);
        String savedSession = prefs.getString(PREF_SESSION, null);

        // Both phones advertise a temporary host code. Comparing the two codes
        // deterministically elects one host and one joiner after they exchange them.
        if (!"host".equals(savedRole) || savedPin == null || !savedPin.matches(PIN_PATTERN)) {
            startHost(null, null);
        } else if (!host || !retryEnabled) {
            startHost(savedPin, savedSession);
        }

        EditText pinInput = new EditText(activity);
        pinInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        pinInput.setFilters(new InputFilter[] { new InputFilter.LengthFilter(PIN_DIGITS) });
        pinInput.setGravity(Gravity.CENTER);
        pinInput.setTextSize(28);
        pinInput.setHint("Enter companion code");

        TextView codeLabel = new TextView(activity);
        codeLabel.setText("Companion code");
        codeLabel.setTextSize(16);
        codeLabel.setPadding(0, 20, 0, 4);

        TextView codeValue = new TextView(activity);
        codeValue.setText(pin);
        codeValue.setTextSize(30);
        codeValue.setGravity(Gravity.CENTER);
        codeValue.setTextIsSelectable(true);

        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = (int) (20 * activity.getResources().getDisplayMetrics().density);
        content.setPadding(padding, padding, padding, padding);
        content.addView(pinInput, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        content.addView(codeLabel);
        content.addView(codeValue, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        AlertDialog dialog = new AlertDialog.Builder(activity)
            .setTitle("Connect another phone")
            .setView(content)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Connect", null)
            .create();
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> {
            String entered = pinInput.getText().toString().trim();
            if (!entered.matches(PIN_PATTERN)) {
                showMessage("Enter all six digits.");
                return;
            }
            if (entered.equals(pin)) {
                startHost(null, null);
                codeValue.setText(pin);
                pinInput.setText("");
                showMessage("The codes must be different. Share the new companion code.");
                return;
            }

            dialog.dismiss();
            if (pin.compareTo(entered) < 0) {
                // The lower code remains the host; the other phone joins it.
                showStatus("Waiting for companion", "Share your companion code with the other phone.");
            } else {
                startJoin(entered);
            }
        }));
        dialog.show();
    }

    private void showPcHostJoinDialog() {
        SharedPreferences prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String rememberedEndpoint = prefs.getString(PREF_PC_HOST_ADDR, BuiltinPcHostConfig.defaultEndpoint());
        String rememberedPin = prefs.getString(PREF_PIN, "");

        float density = activity.getResources().getDisplayMetrics().density;
        int pad = (int) (20 * density);

        EditText pinInput = new EditText(activity);
        pinInput.setInputType(InputType.TYPE_CLASS_NUMBER);
        pinInput.setFilters(new InputFilter[] { new InputFilter.LengthFilter(PIN_DIGITS) });
        pinInput.setGravity(Gravity.CENTER);
        pinInput.setTextSize(28);
        pinInput.setHint("6-digit PIN");
        if (rememberedPin != null && rememberedPin.matches(PIN_PATTERN))
            pinInput.setText(rememberedPin);

        TextView advancedLabel = new TextView(activity);
        advancedLabel.setText("Advanced — PC host (ZeroTier/Tailscale)");
        advancedLabel.setTextSize(13);
        advancedLabel.setPadding(0, (int) (16 * density), 0, (int) (4 * density));

        EditText hostInput = new EditText(activity);
        hostInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        hostInput.setTextSize(14);
        hostInput.setHint(BuiltinPcHostConfig.defaultEndpoint());
        hostInput.setText(rememberedEndpoint != null && !rememberedEndpoint.isEmpty()
            ? rememberedEndpoint : BuiltinPcHostConfig.defaultEndpoint());

        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(pad, pad, pad, pad);
        content.addView(pinInput, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        content.addView(advancedLabel);
        content.addView(hostInput, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        new AlertDialog.Builder(activity)
            .setTitle("Join PC Host")
            .setView(content)
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Connect", (dialog, which) -> {
                String entered = pinInput.getText().toString().trim();
                String endpoint = hostInput.getText().toString().trim();
                if (!entered.matches(PIN_PATTERN)) {
                    showMessage("Enter all six PIN digits.");
                    return;
                }
                if (endpoint.isEmpty())
                    endpoint = BuiltinPcHostConfig.defaultEndpoint();
                startPcHostJoin(entered, endpoint);
            })
            .show();
    }

    private void startPcHostJoin(String enteredPin, String endpoint) {
        closeCurrentSession(false);
        Context serviceContext = appContext != null ? appContext : activity;
        if (serviceContext != null)
            PeerLinkKeepAliveService.start(serviceContext);
        host = false;
        pcServerMode = true;
        pcHelloSent = false;
        retryEnabled = true;
        pin = enteredPin;
        accepted = false;
        directConnected = false;
        nativeConnected = false;
        sessionKey = null;
        pcReconnectToastShown = false;
        lastStatusTitle = null;
        lastPcServerHeartbeatAt = 0;

        if (!parsePcHostEndpoint(endpoint)) {
            showMessage("Host address must look like 10.137.178.161:27770");
            return;
        }

        SharedPreferences prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        pcClientId = prefs.getString("pc_client_id", null);
        if (pcClientId == null || pcClientId.length() < 8) {
            pcClientId = UUID.randomUUID().toString().replace("-", "");
            prefs.edit().putString("pc_client_id", pcClientId).apply();
        }
        String normalized = pcHostAddress + ":" + pcHostPort;
        prefs.edit()
            .putString(PREF_ROLE, "pc")
            .putString(PREF_PIN, pin)
            .putString(PREF_PC_HOST_ADDR, normalized)
            .remove(PREF_SESSION)
            .apply();

        showStatus("Reconnecting…", "Preparing secure link…");
        final String joinPin = pin;
        cryptoExecutor.execute(() -> {
            try {
                byte[] derived = BuiltinPcHostConfig.derivePinKey(joinPin);
                byte[] session = BuiltinPcHostConfig.deriveSessionKey(derived);
                handler.post(() -> {
                    if (!joinPin.equals(pin) || !pcServerMode)
                        return;
                    pinKey = derived;
                    sessionKey = session;
                    handler.removeCallbacks(pcServerWatchdog);
                    handler.postDelayed(pcServerWatchdog, 2000);
                    showStatus("Reconnecting…", "Opening link to the PC host…");
                    ensurePcTcpLink();
                });
            } catch (Exception e) {
                handler.post(() -> showMessage("Could not start PC host link."));
            }
        });
    }

    private boolean parsePcHostEndpoint(String endpoint) {
        if (endpoint == null)
            return false;
        String value = endpoint.trim();
        if (value.isEmpty())
            return false;
        int colon = value.lastIndexOf(':');
        if (colon <= 0 || colon >= value.length() - 1)
            return false;
        String hostPart = value.substring(0, colon).trim();
        String portPart = value.substring(colon + 1).trim();
        try {
            int port = Integer.parseInt(portPart);
            if (hostPart.isEmpty() || port < 1 || port > 65535)
                return false;
            pcHostAddress = hostPart;
            pcHostPort = port;
            return true;
        } catch (Exception ignored) {
            return false;
        }
    }

    private void startHost(String existingPin, String existingSession) {
        closeCurrentSession(false);
        Context serviceContext = appContext != null ? appContext : activity;
        if (serviceContext != null)
            PeerLinkKeepAliveService.start(serviceContext);
        host = true;
        retryEnabled = true;
        pin = existingPin != null && existingPin.matches(PIN_PATTERN) ? existingPin : String.format("%0" + PIN_DIGITS + "d", random.nextInt((int) Math.pow(10, PIN_DIGITS)));
        sessionId = existingSession != null ? existingSession : randomHex(16);
        salt = randomHex(16);
        accepted = false;
        joinSent = false;
        directConnected = false;
        nativeConnected = false;

        final String hostPin = pin;
        final String hostSession = sessionId;
        final String hostSalt = salt;
        showStatus("Your co-op PIN", hostPin + "\nPreparing secure room…");
        cryptoExecutor.execute(() -> {
            try {
                KeyPair pair = createEcdhKeyPair();
                byte[] derived = derivePinKey(hostPin, hexToBytes(hostSalt));
                String pub = Base64.encodeToString(pair.getPublic().getEncoded(), Base64.NO_WRAP);
                handler.post(() -> {
                    if (!hostPin.equals(pin) || !hostSession.equals(sessionId))
                        return;
                    ecdhKeyPair = pair;
                    pinKey = derived;
                    hostPublicKey = pub;
                    Context prefsContext = activity != null ? activity : appContext;
                    if (prefsContext != null) {
                        SharedPreferences prefs = prefsContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
                        prefs.edit().putString(PREF_ROLE, "host").putString(PREF_PIN, hostPin)
                            .putString(PREF_SESSION, hostSession).apply();
                    }
                    connectMqtt(() -> {
                        subscribe(toHostTopic(), relayHostTopic());
                        publishRoomAdvertisement();
                        showStatus("Your co-op PIN", hostPin + "\nWaiting for the other phone…");
                        showHostPinDialog(hostPin);
                    });
                });
            } catch (Exception e) {
                handler.post(() -> showMessage("Could not start co-op: " + e.getMessage()));
            }
        });
    }

    private void showHostPinDialog(String hostPin) {
        Activity current = activity;
        if (current == null || hostPin == null)
            return;
        current.runOnUiThread(() -> {
            TextView value = new TextView(current);
            value.setText(hostPin);
            value.setTextSize(34);
            value.setGravity(Gravity.CENTER);
            value.setTextIsSelectable(true);
            int padding = (int) (24 * current.getResources().getDisplayMetrics().density);
            value.setPadding(padding, padding, padding, padding);
            new AlertDialog.Builder(current)
                .setTitle("Your 6-digit co-op PIN")
                .setMessage("Share this PIN with the other phone. It stays selectable so you can copy it.")
                .setView(value)
                .setPositiveButton("OK", null)
                .show();
        });
    }

    private void startJoin(String enteredPin) {
        closeCurrentSession(false);
        Context serviceContext = appContext != null ? appContext : activity;
        if (serviceContext != null)
            PeerLinkKeepAliveService.start(serviceContext);
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
            final MqttAsyncClient client = new MqttAsyncClient(BROKER, clientId, new MemoryPersistence());
            mqtt = client;
            client.setCallback(new MqttCallbackExtended() {
                @Override public void connectComplete(boolean reconnect, String serverURI) {
                    if (mqtt != client) return;
                    mqttConnected = true;
                    if (pcServerMode) {
                        subscribe(pcRoomTopic());
                        pcHelloSent = false;
                        handler.removeCallbacks(pcHeartbeat);
                        handler.removeCallbacks(pcServerWatchdog);
                        handler.postDelayed(pcServerWatchdog, 10000);
                        // Re-authenticate after a broker reconnect; subscriptions alone
                        // do not restore the PC server's accepted session.
                        handler.post(PeerLinkManager.this::sendPcHello);
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
                    if (mqtt != client) return;
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
                        pcPeerId = null;
                        pcPeerPublicKey = null;
                        sendCounter.set(1);
                        resetReceiveCounters();
                        ecdhKeyPair = null;
                        try {
                            // A broker reconnect starts a fresh ECDH exchange.
                            // Never restart a GCM counter under the previous key.
                            ecdhKeyPair = createEcdhKeyPair();
                        } catch (Exception ignored) {}
                        handler.removeCallbacks(pcHeartbeat);
                    }
                    updateNativeConnection();
                    showStatus("Reconnecting", "Restoring the co-op connection…");
                }

                @Override public void messageArrived(String topic, MqttMessage message) {
                    if (mqtt != client) return;
                    handleMqttMessage(topic, message.getPayload());
                }

                @Override public void deliveryComplete(IMqttDeliveryToken token) {}
            });

            MqttConnectOptions options = new MqttConnectOptions();
            options.setAutomaticReconnect(true);
            options.setCleanSession(true);
            options.setKeepAliveInterval(25);
            options.setConnectionTimeout(15);
            // Live co-op frames use QoS 0; keep inflight headroom for signaling.
            options.setMaxInflight(64);
            client.connect(options, null, new IMqttActionListener() {
                @Override public void onSuccess(IMqttToken token) {
                    if (mqtt != client) return;
                    mqttConnected = true;
                    if (onReady != null)
                        onReady.run();
                }

                @Override public void onFailure(IMqttToken token, Throwable exception) {
                    if (mqtt != client) return;
                    mqttConnected = false;
                    showStatus("Connection service unavailable", "Retrying the co-op rendezvous…");
                    if (retryEnabled)
                        handler.postDelayed(() -> {
                            if (retryEnabled && mqtt == client && !client.isConnected())
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

    private boolean publishJoinMessage() {
        if (host || !mqttConnected || sessionId == null || ecdhKeyPair == null || pinKey == null)
            return false;
        try {
            JSONObject join = new JSONObject();
            String joinPublicKey = Base64.encodeToString(ecdhKeyPair.getPublic().getEncoded(), Base64.NO_WRAP);
            join.put("type", "join");
            join.put("session", sessionId);
            join.put("pub", joinPublicKey);
            join.put("proof", hmacText(pinKey, sessionId + "|" + hostPublicKey + "|" + joinPublicKey));
            return publish(toHostTopic(), join.toString().getBytes(StandardCharsets.UTF_8), 1, false);
        } catch (Exception e) {
            showMessage("Could not join room: " + e.getMessage());
            return false;
        }
    }

    private void handleMqttMessage(String topic, byte[] payload) {
        if (payload == null || payload.length > MAX_MQTT_PAYLOAD_SIZE)
            return;
        try {
            if (pcServerMode) {
                // MQTT is lookup/control only — never ingest game frames from the broker.
                if (topic.equals(pcRoomTopic())) {
                    handlePcRoomAdvertisement(new JSONObject(new String(payload, StandardCharsets.UTF_8)));
                } else if (topic.equals(pcDownControlTopic())) {
                    handlePcServerControl(new JSONObject(new String(payload, StandardCharsets.UTF_8)));
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
        if (!pcServerMode)
            return;
        String advertType = advert.optString("type");
        if (!"world-host".equals(advertType) && !"pc-room".equals(advertType))
            return;
        long timestamp = advert.optLong("timestamp", 0);
        if (timestamp == 0 || System.currentTimeMillis() - timestamp > 30000)
            return;

        long advertTimestamp = advert.optLong("timestamp", 0);
        String incomingSession = advert.getString("session");
        String incomingSalt = advert.getString("salt");
        byte[] candidatePinKey = derivePinKey(pin, hexToBytes(incomingSalt));
        String expectedProof = hmacText(candidatePinKey,
            incomingSession + "|" + incomingSalt + "|" + advertTimestamp);
        if (!constantTimeEquals(expectedProof, advert.optString("proof")))
            return;

        lastPcServerHeartbeatAt = System.currentTimeMillis();
        if (incomingSession.equals(sessionId) && pcHelloSent)
            return;

        sessionId = incomingSession;
        salt = incomingSalt;
        pinKey = candidatePinKey;
        sessionKey = null;
        accepted = false;
        pcPeerId = null;
        pcPeerPublicKey = null;
        if (advert.has("host"))
            pcHostAddress = advert.optString("host", null);
        pcHostPort = advert.optInt("port", advert.optInt("lanPort", BuiltinPcHostConfig.defaultPort()));
        updateNativeConnection();
        sendCounter.set(1);
        resetReceiveCounters();
        ecdhKeyPair = createEcdhKeyPair();
        // Control/lookup only — never subscribe to MQTT game-frame topics.
        subscribe(pcDownControlTopic());
        sendPcHello();
        ensurePcTcpLink();
        showStatus("Reconnecting…", "PIN resolved; opening direct TCP to the PC host…");
    }

    private void ensurePcTcpLink() {
        if (!pcServerMode || pcClientId == null || pinKey == null || !retryEnabled)
            return;
        if (pcHostAddress == null || pcHostAddress.isEmpty()) {
            if (!parsePcHostEndpoint(BuiltinPcHostConfig.defaultEndpoint()))
                return;
        }
        if (pcSocket.isConnected())
            return;
        try {
            String proof = BuiltinPcHostConfig.helloProof(pinKey, pcClientId);
            String hello = "HELLO " + pcClientId + " " + proof;
            pcSocket.connect(pcHostAddress, pcHostPort, hello);
        } catch (Exception ignored) {
            showStatus("Reconnecting…", "Waiting for the PC host…");
        }
    }

    private void sendPcHello() {
        if (!pcServerMode || !mqttConnected || sessionId == null || pinKey == null || ecdhKeyPair == null)
            return;
        try {
            String publicKey = Base64.encodeToString(ecdhKeyPair.getPublic().getEncoded(), Base64.NO_WRAP);
            JSONObject hello = new JSONObject();
            hello.put("type", "hello");
            hello.put("pub", publicKey);
            hello.put("proof", hmacText(pinKey, sessionId + "|" + pcClientId + "|hello"));
            pcHelloSent = publish(pcUpHelloTopic(), hello.toString().getBytes(StandardCharsets.UTF_8), 1, false);
            handler.removeCallbacks(pcHeartbeat);
            if (pcHelloSent)
                handler.postDelayed(pcHeartbeat, 10000);
            else
                handler.postDelayed(this::sendPcHello, 5000);
        } catch (Exception e) {
            showStatus("PC server connection error", e.getMessage());
        }
    }

    private void handlePcServerControl(JSONObject envelope) throws Exception {
        if (!pcServerMode || sessionId == null || pinKey == null
            || !sessionId.equals(envelope.optString("session")))
            return;
        String type = envelope.optString("type");
        String expectedAuth = hmacText(pinKey, sessionId + "|" + pcClientId + "|" + type);
        if (!constantTimeEquals(expectedAuth, envelope.optString("auth")))
            return;
        JSONObject data = envelope.optJSONObject("data");
        if ("server-heartbeat".equals(type)) {
            lastPcServerHeartbeatAt = System.currentTimeMillis();
            if (data != null) {
                if (data.has("host"))
                    pcHostAddress = data.optString("host", pcHostAddress);
                if (data.has("port"))
                    pcHostPort = data.optInt("port", pcHostPort);
                ensurePcTcpLink();
            }
        } else if ("waiting".equals(type)) {
            if (data != null && data.optBoolean("worldHost", false)) {
                applyWorldHostSessionKey(data.optString("sessionKey", null));
                if (data.has("host"))
                    pcHostAddress = data.optString("host", pcHostAddress);
                if (data.has("port"))
                    pcHostPort = data.optInt("port", pcHostPort);
                ensurePcTcpLink();
            }
            showStatus("Reconnecting…", "PC host ready — waiting for companion / direct TCP…");
        } else if ("presence-resync".equals(type)) {
            nativeRequestStoryResync();
        } else if ("peer".equals(type) && data != null) {
            String peerId = data.getString("client");
            boolean worldHost = data.optBoolean("worldHost", false);
            boolean assignedHost = data.optInt("role", 0) == 0;
            if (accepted && sessionKey != null && peerId.equals(pcPeerId))
                return;
            accepted = false;
            if (worldHost) {
                String proof = data.optString("proof", "");
                String expected = hmacText(pinKey, sessionId + "|" + pcClientId + "|peer");
                if (!constantTimeEquals(expected, proof))
                    return;
                applyWorldHostSessionKey(data.optString("sessionKey", null));
                pcPeerPublicKey = "world-host";
                if (data.has("host"))
                    pcHostAddress = data.optString("host", pcHostAddress);
                if (data.has("port"))
                    pcHostPort = data.optInt("port", pcHostPort);
            } else {
                String peerPublicKey = data.getString("pub");
                String proof = data.getString("proof");
                String expected = hmacText(pinKey, sessionId + "|" + peerId + "|" + peerPublicKey);
                if (!constantTimeEquals(expected, proof))
                    return;
                sessionKey = null;
                byte[] shared = deriveSharedSecret(ecdhKeyPair, peerPublicKey);
                sessionKey = deriveSessionKey(shared, pinKey, sessionId);
                pcPeerPublicKey = peerPublicKey;
            }
            pcPeerId = peerId;
            host = assignedHost;
            sendCounter.set(1);
            resetReceiveCounters();
            accepted = true;
            reopenNativeSession();
            // PC host: game data is direct TCP only — never WebRTC/TURN/MQTT frames.
            ensurePcTcpLink();
            if (pcTcpConnected)
                showStatus("Connected to PC host", "Both phones linked through the PC.");
            else
                showStatus("Reconnecting…", "Companion ready; opening direct TCP…");
        } else if ("peer-disconnected".equals(type)) {
            boolean partnerReturning = data != null && data.optBoolean("reconnecting", false);
            handler.removeCallbacks(directTimeout);
            handler.removeCallbacks(directRetry);
            try { if (peerConnection != null) peerConnection.close(); } catch (Exception ignored) {}
            try { if (dataChannel != null) dataChannel.close(); } catch (Exception ignored) {}
            peerConnection = null;
            dataChannel = null;
            directConnected = false;
            pcPeerId = null;
            pcPeerPublicKey = null;
            pendingCandidates.clear();
            if (pcServerMode && partnerReturning) {
                // Stay authenticated to the PC host; only the companion slot is empty.
                updateNativeConnection();
                showStatus("Reconnecting…", "Partner reconnecting — staying on the PC host…");
                return;
            }
            accepted = false;
            sessionKey = null;
            sendCounter.set(1);
            resetReceiveCounters();
            ecdhKeyPair = null;
            try {
                // A returning peer may reuse its previous public key. Rotate
                // our ephemeral key before accepting another peer packet.
                ecdhKeyPair = createEcdhKeyPair();
            } catch (Exception ignored) {}
            pcHelloSent = false;
            updateNativeConnection();
            if (pcServerMode && mqttConnected)
                sendPcHello();
            showStatus(pcServerMode ? "Reconnecting…" : "Relay",
                "Partner left — holding the room. Re-pairs when they return.");
        } else if ("full".equals(type) || "rejected".equals(type)) {
            showStatus("PC server refused connection", data != null ? data.optString("reason", "Unable to join this room.") : "Unable to join this room.");
        }
    }

    /** Shared room AES key from the PC world host (Base64) or local HMAC fallback. */
    private void applyWorldHostSessionKey(String sessionKeyB64) throws Exception {
        if (sessionKeyB64 != null && sessionKeyB64.length() > 0) {
            byte[] decoded = Base64.decode(sessionKeyB64, Base64.DEFAULT);
            if (decoded != null && decoded.length >= 16) {
                sessionKey = decoded;
                return;
            }
        }
        if (pinKey == null || sessionId == null)
            return;
        // Must match HeartSoulWorldHost DeriveSessionKey.
        sessionKey = hmac(pinKey, (sessionId + "|HNS-WORLD-HOST-V1").getBytes(StandardCharsets.UTF_8));
    }

    /**
     * WebRTC signaling from the PC world host on signal/{client}/down.
     * Payload shape matches {@link #sendSignal} for pcServerMode (sdp / candidate fields),
     * not the phone-to-phone mac-wrapped envelope.
     */
    private void handleWorldHostSignal(JSONObject signal) throws Exception {
        if (!pcServerMode || sessionKey == null)
            return;
        String type = signal.optString("type");
        if ("answer".equals(type)) {
            String sdp = signal.optString("sdp");
            if (peerConnection == null || sdp.isEmpty())
                return;
            SessionDescription answer = new SessionDescription(SessionDescription.Type.ANSWER, sdp);
            peerConnection.setRemoteDescription(new SimpleSdpObserver() {
                @Override public void onSetSuccess() {
                    flushPendingCandidates();
                }
            }, answer);
        } else if ("offer".equals(type)) {
            // Host-initiated re-offer after ICE restart; phone answers.
            String sdp = signal.optString("sdp");
            if (sdp.isEmpty())
                return;
            if (peerConnection == null)
                createPeerConnection(false);
            if (peerConnection == null)
                return;
            SessionDescription offer = new SessionDescription(SessionDescription.Type.OFFER, sdp);
            peerConnection.setRemoteDescription(new SimpleSdpObserver() {
                @Override public void onSetSuccess() {
                    flushPendingCandidates();
                    createAnswer();
                }
            }, offer);
        } else if ("ice".equals(type)) {
            String candidate = signal.optString("candidate");
            if (candidate.isEmpty())
                return;
            IceCandidate ice = new IceCandidate(
                signal.optString("sdpMid", "0"),
                signal.optInt("sdpMLineIndex", 0),
                candidate);
            if (peerConnection != null && peerConnection.getRemoteDescription() != null)
                peerConnection.addIceCandidate(ice);
            else
                pendingCandidates.add(ice);
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
            showStatus("PIN not accepted", "The room could not be authenticated. Check the six-digit PIN.");
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
        joinSent = publishJoinMessage();
        showStatus("Pairing", "Authenticating the other phone…");
    }

    private boolean publishAcceptMessage() {
        if (!host || !mqttConnected || acceptedJoinPublicKey == null || sessionId == null || pinKey == null)
            return false;
        try {
            JSONObject accept = new JSONObject();
            accept.put("type", "accept");
            accept.put("session", sessionId);
            accept.put("proof", hmacText(pinKey, sessionId + "|accept|" + acceptedJoinPublicKey));
            return publish(toJoinTopic(), accept.toString().getBytes(StandardCharsets.UTF_8), 1, false);
        } catch (Exception ignored) {
            return false;
        }
    }

    private void handleJoinMessage(JSONObject join) throws Exception {
        if (!"join".equals(join.optString("type")) || sessionId == null
            || !sessionId.equals(join.optString("session")) || (accepted && directConnected))
            return;

        String joinPublicKey = join.getString("pub");
        String expected = hmacText(pinKey, sessionId + "|" + hostPublicKey + "|" + joinPublicKey);
        if (!constantTimeEquals(expected, join.getString("proof")))
            return;

        // A duplicate authenticated join should resend acceptance, not tear down
        // an already-negotiating peer connection.
        if (accepted && host && joinPublicKey.equals(acceptedJoinPublicKey)) {
            publishAcceptMessage();
            return;
        }

        handler.removeCallbacks(acceptRetry);
        resetPeerConnection();
        accepted = false;
        byte[] shared = deriveSharedSecret(ecdhKeyPair, joinPublicKey);
        sessionKey = deriveSessionKey(shared, pinKey, sessionId);
        accepted = true;
        acceptedJoinPublicKey = joinPublicKey;
        acceptRetryAttempts = 0;
        publishAcceptMessage();
        handler.postDelayed(acceptRetry, 2500);

        reopenNativeSession();
        beginDirectLink(true);
    }

    private void handleSignal(JSONObject signal) throws Exception {
        if (!sessionId.equals(signal.optString("session")))
            return;
        String type = signal.optString("type");
        if ("accept".equals(type) && !host && !pcServerMode) {
            if (accepted && sessionKey != null)
                return;
            String expected = hmacText(pinKey, sessionId + "|accept|" + Base64.encodeToString(ecdhKeyPair.getPublic().getEncoded(), Base64.NO_WRAP));
            if (!constantTimeEquals(expected, signal.getString("proof")))
                return;
            byte[] shared = deriveSharedSecret(ecdhKeyPair, hostPublicKey);
            sessionKey = deriveSessionKey(shared, pinKey, sessionId);
            accepted = true;
            reopenNativeSession();
            beginDirectLink(false);
            return;
        }

        String value = signal.optString("value");
        if (sessionKey == null || !verifySignal(type, value, signal.optString("mac")))
            return;
        handler.removeCallbacks(acceptRetry);
        acceptedJoinPublicKey = null;

        if ("offer".equals(type) && !host) {
            if (peerConnection == null)
                createPeerConnection(false);
            if (peerConnection == null)
                return;
            SessionDescription offer = new SessionDescription(SessionDescription.Type.OFFER, value);
            peerConnection.setRemoteDescription(new SimpleSdpObserver() {
                @Override public void onSetSuccess() {
                    flushPendingCandidates();
                    createAnswer();
                }
            }, offer);
        } else if ("answer".equals(type) && host) {
            if (peerConnection == null)
                return;
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
            List<PeerConnection.IceServer> iceServers = buildIceServers();
            PeerConnection.RTCConfiguration config = new PeerConnection.RTCConfiguration(iceServers);
            config.sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN;
            /* ALL keeps host/srflx and relay; never restrict away from TURN. */
            config.iceTransportsType = PeerConnection.IceTransportsType.ALL;
            config.continualGatheringPolicy =
                PeerConnection.ContinualGatheringPolicy.GATHER_CONTINUALLY;
            config.iceCandidatePoolSize = 2;
            peerConnection = factory.createPeerConnection(config, new PeerConnection.Observer() {
                @Override public void onSignalingChange(PeerConnection.SignalingState state) {}
                @Override public void onIceConnectionChange(PeerConnection.IceConnectionState state) {
                    boolean iceReady = state == PeerConnection.IceConnectionState.CONNECTED
                        || state == PeerConnection.IceConnectionState.COMPLETED;
                    boolean channelOpen = dataChannel != null && dataChannel.state() == DataChannel.State.OPEN;
                    directConnected = iceReady && channelOpen;
                    updateNativeConnection();
                    if (directConnected) {
                        handler.removeCallbacks(directTimeout);
                        handler.removeCallbacks(directRetry);
                        announceDirectLink();
                    } else if (accepted && (state == PeerConnection.IceConnectionState.FAILED
                        || state == PeerConnection.IceConnectionState.DISCONNECTED
                        || state == PeerConnection.IceConnectionState.CLOSED)) {
                        if (pcServerMode) {
                            // Keep accepted + MQTT host path; only rebuild WebRTC.
                            directConnected = false;
                            updateNativeConnection();
                            showStatus("PC Host",
                                "WebRTC blip — host path still live; reconnecting WebRTC…");
                            handler.removeCallbacks(directRetry);
                            handler.postDelayed(directRetry, 2000);
                        } else {
                            showStatus("Reconnecting…",
                                "Direct phone link dropped — game data is on the broker relay.");
                            handler.removeCallbacks(directRetry);
                            handler.postDelayed(directRetry, 5000);
                        }
                    }
                }
                @Override public void onIceConnectionReceivingChange(boolean receiving) {}
                @Override public void onIceGatheringChange(PeerConnection.IceGatheringState state) {}
                @Override public void onSelectedCandidatePairChanged(CandidatePairChangeEvent event) {
                    if (event == null)
                        return;
                    refreshDirectViaTurnFromPair(event.local, event.remote);
                    if (directConnected)
                        announceDirectLink();
                }
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
                /* One reliable ordered channel carries movement, mailbox, and
                 * SIO words. Unreliable would drop link-cable transfers. */
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
            showStatus("Reconnecting…", "Direct phone link unavailable — game data is on the broker relay.");
        }
    }

    private void beginDirectLink(boolean offerer) {
        if (pcServerMode)
            return; // Game data is ZeroTier TCP only — never WebRTC/TURN.
        if (!accepted || sessionKey == null)
            return;
        subscribe(signalInTopic());
        handler.removeCallbacks(directTimeout);
        handler.removeCallbacks(directRetry);
        directViaTurn = false;
        handler.postDelayed(directTimeout, DIRECT_TIMEOUT_MS);
        showStatus("Reconnecting…", "Waiting for the peer link…");
        createPeerConnection(offerer);
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
                boolean iceReady = peerConnection != null
                    && (peerConnection.iceConnectionState() == PeerConnection.IceConnectionState.CONNECTED
                        || peerConnection.iceConnectionState() == PeerConnection.IceConnectionState.COMPLETED);
                directConnected = channel.state() == DataChannel.State.OPEN && iceReady;
                if (directConnected) {
                    handler.removeCallbacks(directTimeout);
                    handler.removeCallbacks(directRetry);
                    announceDirectLink();
                } else if (accepted && channel.state() != DataChannel.State.CONNECTING) {
                    showStatus("Reconnecting…",
                        "Direct phone link unavailable — game data is on the broker relay.");
                    handler.removeCallbacks(directRetry);
                    handler.postDelayed(directRetry, 5000);
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
        synchronized (pendingCandidates) {
            for (IceCandidate candidate : pendingCandidates)
                peerConnection.addIceCandidate(candidate);
            pendingCandidates.clear();
        }
    }

    private void sendSignal(String type, String value) {
        try {
            if (pcServerMode) {
                // World host signaling: plain offer/ice JSON on signal/.../up.
                JSONObject message = new JSONObject();
                if ("offer".equals(type) || "answer".equals(type)) {
                    message.put("type", type);
                    message.put("sdp", value);
                } else if ("ice".equals(type)) {
                    JSONObject candidate = new JSONObject(value);
                    message.put("type", "ice");
                    message.put("candidate", candidate.getString("sdp"));
                    message.put("sdpMid", candidate.optString("mid", "0"));
                    message.put("sdpMLineIndex", candidate.optInt("line", 0));
                } else {
                    return;
                }
                publish(pcSignalUpTopic(), message.toString().getBytes(StandardCharsets.UTF_8), 1, false);
                return;
            }
            JSONObject message = new JSONObject();
            message.put("type", type);
            message.put("session", sessionId);
            message.put("value", value);
            message.put("mac", hmacText(sessionKey, sessionId + "|" + type + "|" + value));
            publish(signalOutTopic(), message.toString().getBytes(StandardCharsets.UTF_8), 1, false);
        } catch (Exception ignored) {}
    }

    private boolean verifySignal(String type, String value, String mac) {
        try {
            return constantTimeEquals(hmacText(sessionKey, sessionId + "|" + type + "|" + value), mac);
        } catch (Exception ignored) {
            return false;
        }
    }

    private static int peerFrameType(byte[] frame) {
        if (frame == null || frame.length < PEER_HDR_TYPE_OFFSET + 2)
            return -1;
        return ((frame[PEER_HDR_TYPE_OFFSET] & 0xFF) << 8)
            | (frame[PEER_HDR_TYPE_OFFSET + 1] & 0xFF);
    }

    private static ThreadPoolExecutor senderForFrame(byte[] frame) {
        int type = peerFrameType(frame);
        if (type == MPEER_MSG_SIO_MULTI)
            return sioFrameSender;
        if (type == MPEER_MSG_POSITION)
            return positionSender;
        return mailboxSender;
    }

    public static boolean sendFrame(byte[] frame) {
        PeerLinkManager manager = instance;
        if (manager == null || frame == null || frame.length > MAX_PEER_FRAME_SIZE
            || !manager.accepted || manager.sessionKey == null)
            return false;
        final byte[] queuedFrame = frame.clone();
        final byte[] queuedSessionKey = manager.sessionKey;
        ThreadPoolExecutor executor = senderForFrame(frame);
        try {
            executor.execute(() -> manager.sendFrameInternal(queuedFrame, queuedSessionKey));
            return true;
        } catch (RejectedExecutionException full) {
            return false;
        }
    }

    private synchronized boolean sendFrameInternal(byte[] frame, byte[] expectedSessionKey) {
        if (!accepted || sessionKey == null || sessionKey != expectedSessionKey || frame == null)
            return false;
        try {
            byte[] encrypted = encryptFrame(frame);
            if (pcServerMode) {
                // HARD RULE: game data is direct ZeroTier TCP to the PC host only.
                if (pcSocket.send(encrypted))
                    return true;
                ensurePcTcpLink();
                return false;
            }
            if (directConnected && dataChannel != null && dataChannel.state() == DataChannel.State.OPEN) {
                if (dataChannel.send(new DataChannel.Buffer(ByteBuffer.wrap(encrypted), true)))
                    return true;
            }
            if (!mqttConnected)
                return false;
            return publish(host ? relayJoinTopic() : relayHostTopic(), encrypted, 0, false);
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

    private synchronized void handleEncryptedFrame(byte[] payload) {
        if (sessionKey == null || payload == null || payload.length < 28
            || payload.length > MAX_ENCRYPTED_FRAME_SIZE)
            return;
        try {
            ByteBuffer input = ByteBuffer.wrap(payload);
            int sender = input.getInt();
            long counter = input.getLong();
            // 0/1 = legacy phone roles; 2 = PC World Host authoritative stream.
            if (sender < 0 || sender > 2 || counter <= receiveCounters[sender])
                return;
            // PC-host mode: only accept host-stamped frames (sender 2). Phones
            // must never apply a peer's raw ciphertext (would diverge from SoT).
            if (pcServerMode && sender != 2)
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

    private synchronized void updateNativeConnection() {
        boolean connected = pcServerMode
            ? (accepted && sessionKey != null)
            : (accepted && (mqttConnected || directConnected));
        if (connected != nativeConnected) {
            nativeConnected = connected;
            if (connected)
                nativePeerConnected(host);
            else
                nativePeerDisconnected();
        }
    }

    /* A peer packet can set accepted false and true again before the core
     * thread observes the gap. Force the native disconnect so the role and
     * the SIO queues are rebuilt for this session. */
    private synchronized void reopenNativeSession() {
        if (nativeConnected) {
            nativeConnected = false;
            nativePeerDisconnected();
        }
        updateNativeConnection();
    }

    private void subscribe(String... topics) {
        if (mqtt == null || !mqtt.isConnected())
            return;
        for (String topic : topics) {
            try { mqtt.subscribe(topic, 1); } catch (Exception ignored) {}
        }
    }

    private boolean publish(String topic, byte[] payload, int qos, boolean retained) {
        if (mqtt == null || !mqtt.isConnected())
            return false;
        try {
            MqttMessage message = new MqttMessage(payload);
            message.setQos(qos);
            message.setRetained(retained);
            mqtt.publish(topic, message);
            return true;
        } catch (Exception ignored) {
            return false;
        }
    }

    private byte[] pcAuthSignal(String action) {
        try {
            long timestamp = System.currentTimeMillis();
            String proof = hmacText(pinKey,
                sessionId + "|" + pcClientId + "|" + action + "|" + timestamp);
            return (timestamp + "|" + proof).getBytes(StandardCharsets.UTF_8);
        } catch (Exception ignored) {
            return new byte[0];
        }
    }

    private String pcRootTopic() { return "hns/johto-world/" + pin + "/" + sessionId; }
    private String pcRoomTopic() { return "hns/johto-world/" + pin + "/room"; }
    private String pcUpHelloTopic() { return pcRootTopic() + "/up/" + pcClientId + "/hello"; }
    private String pcUpHeartbeatTopic() { return pcRootTopic() + "/up/" + pcClientId + "/heartbeat"; }
    private String pcUpFrameTopic() { return pcRootTopic() + "/up/" + pcClientId + "/frame"; }
    private String pcUpLeaveTopic() { return pcRootTopic() + "/up/" + pcClientId + "/leave"; }
    private String pcDownControlTopic() { return pcRootTopic() + "/down/" + pcClientId + "/control"; }
    private String pcDownFrameTopic() { return pcRootTopic() + "/down/" + pcClientId + "/frame"; }
    private String pcSignalUpTopic() { return pcRootTopic() + "/signal/" + pcClientId + "/up"; }
    private String pcSignalDownTopic() { return pcRootTopic() + "/signal/" + pcClientId + "/down"; }

    private String roomTopic() { return "hns/johto/" + pin + "/room"; }
    private String toHostTopic() { return "hns/johto/" + pin + "/" + sessionId + "/to-host"; }
    private String toJoinTopic() { return "hns/johto/" + pin + "/" + sessionId + "/to-join"; }
    private String relayHostTopic() { return "hns/johto/" + pin + "/" + sessionId + "/relay/host"; }
    private String relayJoinTopic() { return "hns/johto/" + pin + "/" + sessionId + "/relay/join"; }

    private String signalOutTopic() {
        if (pcServerMode)
            return pcSignalUpTopic();
        return host ? toJoinTopic() : toHostTopic();
    }

    private String signalInTopic() {
        if (pcServerMode)
            return pcSignalDownTopic();
        return host ? toHostTopic() : toJoinTopic();
    }

    private void initializeWebRtc() {
        Context ctx = appContext != null ? appContext : activity;
        if (ctx == null)
            throw new IllegalStateException("No Android context for WebRTC");
        if (!webRtcInitialized) {
            PeerConnectionFactory.initialize(
                PeerConnectionFactory.InitializationOptions.builder(ctx).createInitializationOptions());
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
        // Invalidate queued sends before the nonce counter is reset.
        accepted = false;
        sessionKey = null;
        updateNativeConnection();
        try { if (peerConnection != null) peerConnection.close(); } catch (Exception ignored) {}
        try { if (dataChannel != null) dataChannel.close(); } catch (Exception ignored) {}
        peerConnection = null;
        dataChannel = null;
        directConnected = false;
        directViaTurn = false;
        pendingCandidates.clear();
        sendCounter.set(1);
        resetReceiveCounters();
    }

    private void closeCurrentSession(boolean clearPreferences) {
        positionSender.getQueue().clear();
        mailboxSender.getQueue().clear();
        sioFrameSender.getQueue().clear();
        boolean wasConnected = nativeConnected;
        retryEnabled = false;
        Activity currentActivity = activity;
        if (currentActivity != null)
            PeerLinkKeepAliveService.stop(currentActivity);
        handler.removeCallbacks(roomHeartbeat);
        handler.removeCallbacks(acceptRetry);
        handler.removeCallbacks(pcHeartbeat);
        handler.removeCallbacks(pcServerWatchdog);
        handler.removeCallbacks(directTimeout);
        handler.removeCallbacks(directRetry);
        if (host && mqtt != null && mqtt.isConnected() && pin != null && sessionId != null)
            publish(roomTopic(), new byte[0], 1, true);
        pcSocket.disconnect();
        pcTcpConnected = false;
        pcHostAddress = null;
        pcReconnectToastShown = false;
        lastStatusTitle = null;
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
        acceptedJoinPublicKey = null;
        acceptRetryAttempts = 0;
        pcPeerId = null;
        pcPeerPublicKey = null;
        pinKey = null;
        ecdhKeyPair = null;
        pendingCandidates.clear();
        sendCounter.set(1);
        resetReceiveCounters();
        if (clearPreferences && activity != null) {
            SharedPreferences prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
            String persistentPcClientId = prefs.getString("pc_client_id", null);
            SharedPreferences.Editor editor = prefs.edit().clear();
            if (persistentPcClientId != null)
                editor.putString("pc_client_id", persistentPcClientId);
            editor.apply();
        }
        if (wasConnected)
            nativePeerDisconnected();
    }

    private void showStatus(String title, String message) {
        // No HnsCoop / logcat debug hooks. Deduplicate toasts to avoid flapping.
        if (title != null && title.equals(lastStatusTitle))
            return;
        if ("Reconnecting…".equals(title)) {
            if (pcReconnectToastShown)
                return;
            pcReconnectToastShown = true;
        } else if ("Connected to PC host".equals(title)) {
            pcReconnectToastShown = false;
        }
        lastStatusTitle = title;
        Activity current = activity;
        if (current == null)
            return;
        // Status updates must never create a modal window over the SDL surface.
        final String toast = title;
        current.runOnUiThread(() ->
            Toast.makeText(current, toast, Toast.LENGTH_SHORT).show());
    }

    private void showMessage(String message) {
        Activity current = activity;
        if (current != null)
            current.runOnUiThread(() ->
                Toast.makeText(current, message, Toast.LENGTH_SHORT).show());
    }

    private static class SimpleSdpObserver implements SdpObserver {
        @Override public void onCreateSuccess(SessionDescription description) {}
        @Override public void onSetSuccess() {}
        @Override public void onCreateFailure(String error) {}
        @Override public void onSetFailure(String error) {}
    }

    private static native void nativeInitialize();
    private static native void nativeRequestStoryResync();
    private static native void nativeSetDiagLogs(boolean enabled);
    private native void nativePeerConnected(boolean hostRole);
    private native void nativePeerDisconnected();
    private native void nativePeerFrame(byte[] frame);
}
