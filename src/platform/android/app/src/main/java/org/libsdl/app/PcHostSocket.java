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
 * Direct TCP game-data link to the PC World Host over ZeroTier.
 * No MQTT / WebRTC / TURN. Secrets are never logged.
 */
final class PcHostSocket {
    interface Listener {
        void onReady(int role, int players);
        void onConnected();
        void onDisconnected();
        void onFrame(byte[] encrypted);
        void onHostBusy();
    }

    private static final int MAX_FRAME = 8192;
    private static final int CONNECT_TIMEOUT_MS = 8000;

    private final ExecutorService io = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "HNS-PcHost-TCP");
        t.setDaemon(true);
        return t;
    });
    private final AtomicBoolean running = new AtomicBoolean(false);
    private volatile Socket socket;
    private volatile DataOutputStream out;
    private volatile Listener listener;
    private volatile String host;
    private volatile int port;
    private volatile String helloLine;

    void setListener(Listener listener) {
        this.listener = listener;
    }

    synchronized void connect(String host, int port, String helloLine) {
        this.host = host;
        this.port = port;
        this.helloLine = helloLine;
        disconnectLocked();
        running.set(true);
        io.execute(this::runLoop);
    }

    synchronized void disconnect() {
        running.set(false);
        disconnectLocked();
    }

    boolean isConnected() {
        Socket s = socket;
        return s != null && s.isConnected() && !s.isClosed();
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
            running.set(false);
            return false;
        }
    }

    private void runLoop() {
        while (running.get()) {
            Socket s = null;
            try {
                s = new Socket();
                s.setTcpNoDelay(true);
                s.setKeepAlive(true);
                s.connect(new InetSocketAddress(host, port), CONNECT_TIMEOUT_MS);
                DataOutputStream output = new DataOutputStream(s.getOutputStream());
                InputStream rawIn = s.getInputStream();
                output.write((helloLine + "\n").getBytes(StandardCharsets.UTF_8));
                output.flush();

                // Byte-at-a-time so we never buffer past the READY line into frame data.
                String ready = readLineExact(rawIn);
                if (ready == null)
                    throw new IOException("closed before READY");
                if (ready.startsWith("FULL")) {
                    Listener busy = listener;
                    if (busy != null)
                        busy.onHostBusy();
                    running.set(false);
                    break;
                }
                if (!ready.startsWith("READY "))
                    throw new IOException("bad handshake");
                String[] parts = ready.split(" ");
                int role = parts.length > 1 ? Integer.parseInt(parts[1]) : 0;
                int players = parts.length > 2 ? Integer.parseInt(parts[2]) : 1;

                DataInputStream input = new DataInputStream(rawIn);
                synchronized (this) {
                    socket = s;
                    out = output;
                }
                Listener l = listener;
                if (l != null) {
                    l.onReady(role, players);
                    l.onConnected();
                }

                while (running.get()) {
                    int len = input.readInt();
                    if (len < 16 || len > MAX_FRAME)
                        break;
                    byte[] frame = new byte[len];
                    input.readFully(frame);
                    Listener recv = listener;
                    if (recv != null)
                        recv.onFrame(frame);
                }
            } catch (Exception ignored) {
            } finally {
                try { if (s != null) s.close(); } catch (Exception ignored) {}
                synchronized (this) {
                    if (socket == s) {
                        socket = null;
                        out = null;
                    }
                }
                Listener l = listener;
                if (l != null && running.get())
                    l.onDisconnected();
            }
            if (!running.get())
                break;
            try { Thread.sleep(2000); } catch (InterruptedException ignored) { break; }
        }
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
