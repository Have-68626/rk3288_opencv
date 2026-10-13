package com.example.rk3288_opencv;

import android.app.Activity;
import android.view.View;
import android.widget.AdapterView;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.RadioGroup;
import android.widget.Spinner;
import android.widget.SpinnerAdapter;
import android.widget.Switch;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.floatingactionbutton.FloatingActionButton;

final class MainScreenBinder {
    interface Callbacks {
        void onStartStopClicked();

        void onViewLogsClicked();

        void onNavPlaybackClicked();

        void onNavSettingsClicked();

        void onNavAboutClicked();

        void onExitClicked();

        void onSetMockUrlClicked();

        void onPushRtmpClicked();

        void onStopRtmpClicked();

        void onHotRestartClicked();

        void onModeChanged(int checkedId);

        void onToggleRecognitionPanelClicked();

        boolean isCameraSpinnerInitialized();

        void setCameraSpinnerInitialized(boolean initialized);

        void onCameraSpinnerItemSelected(int position);

        void onCaptureAutoChanged(boolean enabled);

        void onCaptureSchemeChanged(@NonNull CaptureScheme scheme);
    }

    static final class Binding {
        ImageView monitorView;
        android.view.SurfaceView previewSurface;
        View videoWrapper;
        TextView tvFps;
        TextView tvCpu;
        TextView tvMemory;
        TextView tvLatency;
        TextView tvCaptureStrategyInfo;
        TextView tvStatus;
        TextView tvOverlayStatus;
        View recognitionTitle;

        Button btnStartStop;
        Button btnViewLogs;
        Button btnNavPlayback;
        Button btnNavSettings;
        Button btnNavAbout;
        Button btnExit;
        Button btnHotRestart;

        RadioGroup rgMode;
        RadioGroup rgCaptureScheme;
        RadioGroup rgDetectionThrottle;
        RadioGroup rgInferenceThrottle;
        Switch switchCaptureAuto;
        Spinner spinnerCameras;
        Switch switchFlipX;
        Switch switchFlipY;
        Switch switchAccelOpenCL;
        Switch switchAccelMpp;
        Switch switchAccelQualcomm;
        Switch switchOverlay;

        View panelSettings;

        EditText etDetectionIntervalMs;
        TextView tvDetectionIntervalEffective;
        EditText etInferenceIntervalMs;
        TextView tvInferenceIntervalEffective;

        EditText etMockUrl;
        Button btnSetMockUrl;

        EditText etRtmpUrl;
        Button btnPushRtmp;
        Button btnStopRtmp;

        View panelRecognitionEvents;
        RecyclerView rvRecognitionEvents;
        FloatingActionButton fabToggleEvents;
    }

    @NonNull
    static Binding bindViews(@NonNull Activity activity) {
        Binding b = new Binding();
        b.monitorView = activity.findViewById(R.id.monitor_view);
        b.previewSurface = activity.findViewById(R.id.preview_surface);
        b.videoWrapper = activity.findViewById(R.id.video_wrapper);

        b.tvFps = activity.findViewById(R.id.tv_fps);
        b.tvCpu = activity.findViewById(R.id.tv_cpu);
        b.tvMemory = activity.findViewById(R.id.tv_memory);
        b.tvLatency = activity.findViewById(R.id.tv_latency);
        b.tvCaptureStrategyInfo = activity.findViewById(R.id.tv_capture_strategy_info);
        b.tvStatus = activity.findViewById(R.id.tv_app_status);
        b.tvOverlayStatus = activity.findViewById(R.id.tv_overlay_status);

        b.recognitionTitle = activity.findViewById(R.id.recognition_title);
        b.btnStartStop = activity.findViewById(R.id.btn_start_stop);
        b.btnViewLogs = activity.findViewById(R.id.btn_view_logs);
        b.btnNavPlayback = activity.findViewById(R.id.btn_nav_playback);
        b.btnNavSettings = activity.findViewById(R.id.btn_nav_settings);
        b.btnNavAbout = activity.findViewById(R.id.btn_nav_about);
        b.btnExit = activity.findViewById(R.id.btn_exit);
        b.btnHotRestart = activity.findViewById(R.id.btn_hot_restart);

        b.rgMode = activity.findViewById(R.id.rg_mode);
        b.rgCaptureScheme = activity.findViewById(R.id.rg_capture_scheme);
        b.rgDetectionThrottle = activity.findViewById(R.id.rg_detection_throttle);
        b.rgInferenceThrottle = activity.findViewById(R.id.rg_inference_throttle);
        b.switchCaptureAuto = activity.findViewById(R.id.switch_capture_auto);
        b.spinnerCameras = activity.findViewById(R.id.spinner_cameras);
        b.switchFlipX = activity.findViewById(R.id.switch_flip_x);
        b.switchFlipY = activity.findViewById(R.id.switch_flip_y);
        b.switchAccelOpenCL = activity.findViewById(R.id.switch_accel_opencl);
        b.switchAccelMpp = activity.findViewById(R.id.switch_accel_mpp);
        b.switchAccelQualcomm = activity.findViewById(R.id.switch_accel_qualcomm);
        b.switchOverlay = activity.findViewById(R.id.switch_overlay);

        b.panelSettings = activity.findViewById(R.id.panel_settings);

        b.etDetectionIntervalMs = activity.findViewById(R.id.et_detection_interval_ms);
        b.tvDetectionIntervalEffective = activity.findViewById(R.id.tv_detection_interval_effective);
        b.etInferenceIntervalMs = activity.findViewById(R.id.et_inference_interval_ms);
        b.tvInferenceIntervalEffective = activity.findViewById(R.id.tv_inference_interval_effective);

        b.etMockUrl = activity.findViewById(R.id.et_mock_url);
        b.btnSetMockUrl = activity.findViewById(R.id.btn_set_mock_url);

        b.etRtmpUrl = activity.findViewById(R.id.et_rtmp_url);
        b.btnPushRtmp = activity.findViewById(R.id.btn_push_rtmp);
        b.btnStopRtmp = activity.findViewById(R.id.btn_stop_rtmp);

        b.panelRecognitionEvents = activity.findViewById(R.id.panel_recognition_events);
        b.rvRecognitionEvents = activity.findViewById(R.id.rv_recognition_events);
        b.fabToggleEvents = activity.findViewById(R.id.fab_toggle_events);
        return b;
    }

    static void bindStaticListeners(@NonNull Binding b, @NonNull Callbacks cb) {
        if (b.btnStartStop != null) b.btnStartStop.setOnClickListener(v -> cb.onStartStopClicked());
        if (b.btnViewLogs != null) b.btnViewLogs.setOnClickListener(v -> cb.onViewLogsClicked());
        if (b.btnNavPlayback != null) b.btnNavPlayback.setOnClickListener(v -> cb.onNavPlaybackClicked());
        if (b.btnNavSettings != null) b.btnNavSettings.setOnClickListener(v -> cb.onNavSettingsClicked());
        if (b.btnNavAbout != null) b.btnNavAbout.setOnClickListener(v -> cb.onNavAboutClicked());
        if (b.btnExit != null) b.btnExit.setOnClickListener(v -> cb.onExitClicked());
        if (b.btnSetMockUrl != null) b.btnSetMockUrl.setOnClickListener(v -> cb.onSetMockUrlClicked());
        if (b.btnPushRtmp != null) b.btnPushRtmp.setOnClickListener(v -> cb.onPushRtmpClicked());
        if (b.btnStopRtmp != null) b.btnStopRtmp.setOnClickListener(v -> cb.onStopRtmpClicked());
        if (b.btnHotRestart != null) b.btnHotRestart.setOnClickListener(v -> cb.onHotRestartClicked());

        if (b.rgMode != null) {
            b.rgMode.setOnCheckedChangeListener((group, checkedId) -> cb.onModeChanged(checkedId));
        }
        if (b.fabToggleEvents != null) {
            b.fabToggleEvents.setOnClickListener(v -> cb.onToggleRecognitionPanelClicked());
        }
    }

    static void bindCaptureControls(@NonNull Binding b,
                                    boolean captureAutoEnabled,
                                    @NonNull CaptureScheme preferredScheme,
                                    @NonNull Callbacks cb) {
        if (b.switchCaptureAuto != null) {
            b.switchCaptureAuto.setOnCheckedChangeListener(null);
            b.switchCaptureAuto.setChecked(captureAutoEnabled);
            b.switchCaptureAuto.setOnCheckedChangeListener((buttonView, isChecked) -> cb.onCaptureAutoChanged(isChecked));
        }
        if (b.rgCaptureScheme != null) {
            b.rgCaptureScheme.setOnCheckedChangeListener(null);
            b.rgCaptureScheme.check(preferredScheme == CaptureScheme.CAMERAX ? R.id.rb_capture_camerax : R.id.rb_capture_camera2);
            b.rgCaptureScheme.setOnCheckedChangeListener((group, checkedId) -> {
                CaptureScheme next = (checkedId == R.id.rb_capture_camerax) ? CaptureScheme.CAMERAX : CaptureScheme.CAMERA2;
                cb.onCaptureSchemeChanged(next);
            });
        }
    }

    static void bindCameraSpinner(@NonNull Binding b,
                                  @NonNull SpinnerAdapter adapter,
                                  boolean resetInitialized,
                                  @NonNull Callbacks cb) {
        if (b.spinnerCameras == null) return;
        b.spinnerCameras.setAdapter(adapter);
        if (resetInitialized) {
            cb.setCameraSpinnerInitialized(false);
        }
        b.spinnerCameras.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (!cb.isCameraSpinnerInitialized()) {
                    cb.setCameraSpinnerInitialized(true);
                    return;
                }
                cb.onCameraSpinnerItemSelected(position);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
    }
}

