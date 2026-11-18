package org.libsdl.app;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Single persistent TCP link to the PC World Host.
 * Owns reconnect with backoff. Never starts a second loop while one is alive.
 * Secrets are never logged.
 */
final class PcHostSocket {
    interface Listener {
        /** First successful handshake for this connect session. */
        void onSessionReady(int role, int players);
        /** TCP is up (first connect or soft resume). */
        void onLinkUp();
        /** TCP dropped; socket will retry with backoff unless stopped. */
        void onLinkDown();
        /** Gave up after sustained failures — one terminal signal. */
        void onUnreachable();
        void onFrame(byte[] encrypted);
        void onHostBusy();
        /** Connect/handshake progress for the on-screen status HUD (no secrets). */
        default void onStatus(String detail) {}
    }

    private static final int MAX_FRAME = 8192;
    private static final int CONNECT_TIMEOUT_MS = 8000;
    private static final int MAX_FAIL_STREAK = 8; // ~1+2+4+8+16+30+30+30s ≈ 2 min then stop
    private static final long BACKOFF_START_MS = 1000L;
    private static final long BACKOFF_CAP_MS = 30000L;

    private final ExecutorService io = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "HNS-PcHost-TCP");
        t.setDaemon(true);
        return t;
    });
    private final AtomicBoolean wantRun = new AtomicBoolean(false);
    private final AtomicBoolean loopAlive = new AtomicBoolean(false);
    private final Object gate = new Object();
    private volatile Socket socket;
    private volatile DataOutputStream out;
    private volatile Listener listener;
    private volatile String host;
    private volatile int port;
    private volatile String helloLine;
    private volatile boolean sessionHandshook;

    void setListener(Listener listener) {
        this.listener = listener;
    }

    /**
     * Start or keep a single connection loop. No-op if already linked to the
     * same endpoint. Never queues a second runLoop.
     */
    void connect(String host, int port, String helloLine) {
        synchronized (gate) {
            boolean same = host != null && host.equals(this.host)
                && port == this.port
                && helloLine != null && helloLine.equals(this.helloLine);
            this.host = host;
            this.port = port;
            this.helloLine = helloLine;
            wantRun.set(true);
            if (same && isConnected())
                return; // already on the right link — never reconnect while connected
            // Kick a stale/wrong socket; the existing loop (if any) will reconnect.
            disconnectLocked();
            if (loopAlive.compareAndSet(false, true))
                io.execute(this::runLoop);
        }
    }

    void disconnect() {
        wantRun.set(false);
        sessionHandshook = false;
        synchronized (gate) {
            disconnectLocked();
        }
    }

    /** Force-close the socket so the single loop performs a soft reconnect. */
    void kick() {
        synchronized (gate) {
            disconnectLocked();
        }
    }

    boolean isConnected() {
        Socket s = socket;
        return s != null && s.isConnected() && !s.isClosed();
    }

    boolean isActive() {
        return wantRun.get();
    }

    boolean send(byte[] encrypted) {
        DataOutputStream stream = out;
        if (stream == null || encrypted == null || encrypted.length == 0 || encrypted.length > MAX_FRAME)
            return false;
        try {
            synchronized (stream) {
                stream.writeInt(encrypted.length);
                stream.write(encrypted);
                stream.flush();
            }
            return true;
        } catch (IOException e) {
            kick();
            return false;
        }
    }

    private void runLoop() {
        long backoffMs = BACKOFF_START_MS;
        int failStreak = 0;
        boolean announcedDown = false;
        try {
            while (wantRun.get()) {
                Socket s = null;
                try {
                    status("Connecting to " + host + ":" + port + "…");
                    s = new Socket();
                    s.setTcpNoDelay(true);
                    s.setKeepAlive(true);
                    s.connect(new InetSocketAddress(host, port), CONNECT_TIMEOUT_MS);
                    status("TCP up — sending HELLO…");
                    DataOutputStream output = new DataOutputStream(s.getOutputStream());
                    InputStream rawIn = s.getInputStream();
                    output.write((helloLine + "\n").getBytes(StandardCharsets.UTF_8));
                    output.flush();

                    String ready = readLineExact(rawIn);
                    if (ready == null)
                        throw new IOException("host closed before READY");
                    if (ready.startsWith("FULL")) {
                        status("Host full");
                        Listener busy = listener;
                        if (busy != null)
                            busy.onHostBusy();
                        wantRun.set(false);
                        break;
                    }
                    if (!ready.startsWith("READY "))
                        throw new IOException("unexpected handshake reply");
                    String[] parts = ready.split(" ");
                    int role = parts.length > 1 ? Integer.parseInt(parts[1]) : 0;
                    int players = parts.length > 2 ? Integer.parseInt(parts[2]) : 1;

                    DataInputStream input = new DataInputStream(rawIn);
                    synchronized (gate) {
                        socket = s;
                        out = output;
                    }
                    failStreak = 0;
                    backoffMs = BACKOFF_START_MS;
                    announcedDown = false;
                    Listener l = listener;
                    boolean first = !sessionHandshook;
                    sessionHandshook = true;
                    status("READY role=" + role + " players=" + players);
                    if (l != null) {
                        if (first)
                            l.onSessionReady(role, players);
                        l.onLinkUp();
                    }

                    while (wantRun.get()) {
                        int len = input.readInt();
                        // Length-prefixed deframer: reject truncated / oversized
                        // frames the same way the host does (16..8192).
                        if (len < 16 || len > MAX_FRAME) {
                            status("Bad frame length " + len + " — reconnecting");
                            break;
                        }
                        byte[] frame = new byte[len];
                        input.readFully(frame);
                        Listener recv = listener;
                        if (recv != null)
                            recv.onFrame(frame);
                    }
                } catch (Exception e) {
                    String reason = e.getMessage();
                    if (reason == null || reason.isEmpty())
                        reason = e.getClass().getSimpleName();
                    // Never include HELLO/proof material — only a short reason.
                    status("Link error: " + reason);
                } finally {
                    try { if (s != null) s.close(); } catch (Exception ignored) {}
                    synchronized (gate) {
                        if (socket == s) {
                            socket = null;
                            out = null;
                        }
                    }
                }

                if (!wantRun.get())
                    break;

                if (!announcedDown) {
                    announcedDown = true;
                    Listener l = listener;
                    if (l != null)
                        l.onLinkDown();
                }

                failStreak++;
                if (failStreak >= MAX_FAIL_STREAK) {
                    wantRun.set(false);
                    Listener l = listener;
                    if (l != null)
                        l.onUnreachable();
                    break;
                }

                try {
                    Thread.sleep(backoffMs);
                } catch (InterruptedException ignored) {
                    break;
                }
                backoffMs = Math.min(backoffMs * 2, BACKOFF_CAP_MS);
            }
        } finally {
            sessionHandshook = false;
            synchronized (gate) {
                disconnectLocked();
            }
            loopAlive.set(false);
            // If connect() raced and re-armed wantRun, start exactly one new loop.
            if (wantRun.get() && loopAlive.compareAndSet(false, true))
                io.execute(this::runLoop);
        }
    }

    private void status(String detail) {
        Listener l = listener;
        if (l != null)
            l.onStatus(detail);
    }

    private static String readLineExact(InputStream in) throws IOException {
        ByteArrayOutputStream buf = new ByteArrayOutputStream(64);
        while (true) {
            int b = in.read();
            if (b < 0)
                return buf.size() == 0 ? null : buf.toString(StandardCharsets.UTF_8.name());
            if (b == '\n')
                break;
            if (b != '\r')
                buf.write(b);
            if (buf.size() > 256)
                throw new IOException("handshake line too long");
        }
        return buf.toString(StandardCharsets.UTF_8.name());
    }

    private void disconnectLocked() {
        try {
            if (socket != null)
                socket.close();
        } catch (Exception ignored) {}
        socket = null;
        out = null;
    }
}
