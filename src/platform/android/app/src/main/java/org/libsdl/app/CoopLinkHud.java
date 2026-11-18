package org.libsdl.app;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * Persistent on-screen co-op link status. Shows host link state and whether a
 * partner has been seen recently. Never logs secrets.
 */
final class CoopLinkHud {
    enum HostState {
        OFFLINE,
        CONNECTING,
        CONNECTED
    }

    private TextView label;
    private HostState hostState = HostState.OFFLINE;
    private boolean partnerOnline;
    private String detail = "";

    synchronized void attach(Activity activity, ViewGroup layout) {
        if (activity == null || layout == null)
            return;
        activity.runOnUiThread(() -> {
            if (label != null && label.getParent() == layout) {
                render();
                return;
            }
            if (label != null && label.getParent() instanceof ViewGroup) {
                ((ViewGroup) label.getParent()).removeView(label);
            }
            label = new TextView(activity);
            label.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
            label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            label.setPadding(dp(activity, 10), dp(activity, 6), dp(activity, 10), dp(activity, 6));
            label.setBackgroundColor(0xCC101820);
            label.setTextColor(Color.WHITE);
            label.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
            FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.START);
            params.leftMargin = dp(activity, 8);
            params.topMargin = dp(activity, 8);
            // RelativeLayout also accepts these LayoutParams as generic MarginLayoutParams
            // when cast isn't FrameLayout; fall back to generating matching params.
            try {
                layout.addView(label, params);
            } catch (ClassCastException wrongParent) {
                ViewGroup.MarginLayoutParams alt = new ViewGroup.MarginLayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                alt.leftMargin = params.leftMargin;
                alt.topMargin = params.topMargin;
                layout.addView(label, alt);
            }
            render();
        });
    }

    synchronized void setHostState(HostState state, String detailText) {
        hostState = state != null ? state : HostState.OFFLINE;
        detail = detailText != null ? detailText : "";
        render();
    }

    synchronized void setPartnerOnline(boolean online) {
        partnerOnline = online;
        render();
    }

    private void render() {
        TextView view = label;
        if (view == null)
            return;
        final String hostLine;
        final int color;
        switch (hostState) {
            case CONNECTED:
                hostLine = "Host: connected";
                color = 0xFF7CFF9A;
                break;
            case CONNECTING:
                hostLine = "Host: connecting…";
                color = 0xFFFFD27C;
                break;
            default:
                hostLine = "Host: offline";
                color = 0xFFFF8A8A;
                break;
        }
        String partnerLine = partnerOnline ? "Partner: online" : "Partner: waiting";
        String text = hostLine + "\n" + partnerLine;
        if (detail != null && !detail.isEmpty() && hostState != HostState.CONNECTED)
            text += "\n" + detail;
        final String shown = text;
        view.post(() -> {
            view.setTextColor(color);
            view.setText(shown);
            view.setVisibility(android.view.View.VISIBLE);
        });
    }

    private static int dp(Activity activity, int value) {
        float density = activity.getResources().getDisplayMetrics().density;
        return Math.round(value * density);
    }
}
