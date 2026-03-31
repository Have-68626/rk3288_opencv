package com.example.rk3288_opencv;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;

import androidx.annotation.NonNull;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

final class PermissionStateMachine {
    enum State {
        INIT,
        REQUESTING,
        GRANTED,
        DENIED_TEMP,
        DENIED_PERM,
        SAFE_MODE
    }

    interface Listener {
        void onStateChanged(@NonNull State state, @NonNull List<String> missingRuntimePermissions);
    }

    private static final int RUNTIME_REQUEST_CODE = 1001;

    private final Activity activity;
    private final Listener listener;

    private State state = State.INIT;
    private List<String> missing = Collections.emptyList();

    PermissionStateMachine(@NonNull Activity activity, @NonNull Listener listener) {
        this.activity = Objects.requireNonNull(activity);
        this.listener = Objects.requireNonNull(listener);
    }

    @NonNull
    State getState() {
        return state;
    }

    boolean isRuntimeGranted() {
        return state == State.GRANTED;
    }

    boolean isOverlayGranted() {
        if (Build.VERSION.SDK_INT < 23) return true;
        return Settings.canDrawOverlays(activity);
    }

    void evaluate() {
        List<String> required = requiredRuntimePermissions();
        List<String> missingNow = new ArrayList<>();
        for (String p : required) {
            if (ContextCompat.checkSelfPermission(activity, p) != PackageManager.PERMISSION_GRANTED) {
                missingNow.add(p);
            }
        }
        missing = missingNow;
        if (missingNow.isEmpty()) {
            setState(State.GRANTED);
        } else {
            if (state == State.INIT) {
                setState(State.SAFE_MODE);
            } else if (state == State.GRANTED) {
                setState(State.SAFE_MODE);
            } else {
                listener.onStateChanged(state, new ArrayList<>(missingNow));
            }
        }
    }

    void requestWithUserConfirmation() {
        evaluate();
        if (missing.isEmpty()) {
            return;
        }

        new AlertDialog.Builder(activity)
                .setTitle("需要权限")
                .setMessage("需要授予相机权限才能启用监控功能。拒绝后将进入安全模式（可查看界面与日志）。")
                .setPositiveButton("继续申请", (d, w) -> requestNow())
                .setNegativeButton("进入安全模式", (d, w) -> setState(State.SAFE_MODE))
                .show();
    }

    void requestNow() {
        evaluate();
        if (missing.isEmpty()) {
            return;
        }
        setState(State.REQUESTING);
        ActivityCompat.requestPermissions(activity, missing.toArray(new String[0]), RUNTIME_REQUEST_CODE);
    }

    void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        if (requestCode != RUNTIME_REQUEST_CODE) return;
        evaluate();
        if (missing.isEmpty()) {
            setState(State.GRANTED);
            return;
        }

        boolean permanentlyDenied = true;
        for (String p : missing) {
            if (ActivityCompat.shouldShowRequestPermissionRationale(activity, p)) {
                permanentlyDenied = false;
                break;
            }
        }
        setState(permanentlyDenied ? State.DENIED_PERM : State.DENIED_TEMP);
    }

    void showGoToSettingsDialogIfNeeded() {
        if (state != State.DENIED_PERM) return;
        new AlertDialog.Builder(activity)
                .setTitle("权限被永久拒绝")
                .setMessage("部分权限被永久拒绝，请到系统设置中开启后再使用监控功能。")
                .setPositiveButton("打开设置", (d, w) -> {
                    Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS);
                    Uri uri = Uri.fromParts("package", activity.getPackageName(), null);
                    intent.setData(uri);
                    activity.startActivity(intent);
                })
                .setNegativeButton("取消", null)
                .show();
    }

    @NonNull
    private List<String> requiredRuntimePermissions() {
        List<String> perms = new ArrayList<>();
        perms.add(Manifest.permission.CAMERA);
        return perms;
    }

    private void setState(@NonNull State s) {
        List<String> snapshot = missing == null ? Collections.emptyList() : new ArrayList<>(missing);
        boolean sameState = (state == s);
        if (sameState) {
            listener.onStateChanged(state, snapshot);
            return;
        }
        state = s;
        listener.onStateChanged(state, snapshot);
    }
}

