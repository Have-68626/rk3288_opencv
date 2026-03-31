package com.example.rk3288_opencv;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.opengl.GLUtils;
import android.util.AttributeSet;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public final class OsdOverlayView extends GLSurfaceView {
    private final OsdRenderer renderer = new OsdRenderer();

    public OsdOverlayView(@NonNull Context context) {
        super(context);
        init();
    }

    public OsdOverlayView(@NonNull Context context, @Nullable AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        setEGLContextClientVersion(2);
        setEGLConfigChooser(8, 8, 8, 8, 16, 0);
        getHolder().setFormat(PixelFormat.TRANSLUCENT);
        setZOrderOnTop(true);
        setRenderer(renderer);
        setRenderMode(RENDERMODE_CONTINUOUSLY);
    }

    public void setWatermark(@NonNull String watermark) {
        renderer.setWatermark(watermark);
    }

    private static final class OsdRenderer implements Renderer {
        private static final float[] VERT = {
                -1f, -1f, 0f, 1f,
                1f, -1f, 1f, 1f,
                -1f, 1f, 0f, 0f,
                1f, 1f, 1f, 0f
        };

        private final FloatBuffer vb;
        private int program;
        private int aPos;
        private int aUv;
        private int uTex;
        private int uVignette;
        private int texId;

        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final SimpleDateFormat sdf = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.CHINA);
        private volatile String watermark = "OSD";
        private long lastSec = -1;
        private Bitmap osdBitmap;

        OsdRenderer() {
            ByteBuffer bb = ByteBuffer.allocateDirect(VERT.length * 4).order(ByteOrder.nativeOrder());
            vb = bb.asFloatBuffer();
            vb.put(VERT).position(0);
            paint.setColor(Color.WHITE);
            paint.setTextSize(36f);
        }

        void setWatermark(@NonNull String watermark) {
            this.watermark = watermark;
        }

        @Override
        public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            program = buildProgram(VS, FS);
            aPos = GLES20.glGetAttribLocation(program, "aPos");
            aUv = GLES20.glGetAttribLocation(program, "aUv");
            uTex = GLES20.glGetUniformLocation(program, "uTex");
            uVignette = GLES20.glGetUniformLocation(program, "uVignette");

            int[] t = new int[1];
            GLES20.glGenTextures(1, t, 0);
            texId = t[0];
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texId);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE);
            GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE);

            osdBitmap = Bitmap.createBitmap(1024, 128, Bitmap.Config.ARGB_8888);
            updateOsdBitmap(true);
            GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, osdBitmap, 0);

            GLES20.glClearColor(0f, 0f, 0f, 0f);
            GLES20.glEnable(GLES20.GL_BLEND);
            GLES20.glBlendFunc(GLES20.GL_SRC_ALPHA, GLES20.GL_ONE_MINUS_SRC_ALPHA);
        }

        @Override
        public void onSurfaceChanged(GL10 gl, int width, int height) {
            GLES20.glViewport(0, 0, width, height);
        }

        @Override
        public void onDrawFrame(GL10 gl) {
            updateOsdBitmap(false);
            if (osdBitmap != null) {
                GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texId);
                GLUtils.texSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, osdBitmap);
            }

            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            GLES20.glUseProgram(program);

            vb.position(0);
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 16, vb);
            GLES20.glEnableVertexAttribArray(aPos);
            vb.position(2);
            GLES20.glVertexAttribPointer(aUv, 2, GLES20.GL_FLOAT, false, 16, vb);
            GLES20.glEnableVertexAttribArray(aUv);

            GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texId);
            GLES20.glUniform1i(uTex, 0);
            GLES20.glUniform1f(uVignette, 0.25f);

            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
        }

        private void updateOsdBitmap(boolean force) {
            long sec = System.currentTimeMillis() / 1000L;
            if (!force && sec == lastSec) return;
            lastSec = sec;
            if (osdBitmap == null) return;
            Canvas c = new Canvas(osdBitmap);
            c.drawColor(Color.TRANSPARENT, android.graphics.PorterDuff.Mode.CLEAR);
            paint.setColor(Color.WHITE);
            c.drawText(watermark, 20f, 48f, paint);
            paint.setColor(0xCCFFFFFF);
            c.drawText(sdf.format(new Date(sec * 1000L)), 20f, 100f, paint);
        }

        private static int buildProgram(String vs, String fs) {
            int v = compile(GLES20.GL_VERTEX_SHADER, vs);
            int f = compile(GLES20.GL_FRAGMENT_SHADER, fs);
            int p = GLES20.glCreateProgram();
            GLES20.glAttachShader(p, v);
            GLES20.glAttachShader(p, f);
            GLES20.glLinkProgram(p);
            int[] link = new int[1];
            GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, link, 0);
            if (link[0] == 0) {
                String log = GLES20.glGetProgramInfoLog(p);
                GLES20.glDeleteProgram(p);
                throw new RuntimeException(log);
            }
            GLES20.glDeleteShader(v);
            GLES20.glDeleteShader(f);
            return p;
        }

        private static int compile(int type, String src) {
            int s = GLES20.glCreateShader(type);
            GLES20.glShaderSource(s, src);
            GLES20.glCompileShader(s);
            int[] ok = new int[1];
            GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0);
            if (ok[0] == 0) {
                String log = GLES20.glGetShaderInfoLog(s);
                GLES20.glDeleteShader(s);
                throw new RuntimeException(log);
            }
            return s;
        }

        private static final String VS =
                "attribute vec2 aPos;" +
                        "attribute vec2 aUv;" +
                        "varying vec2 vUv;" +
                        "void main(){" +
                        "vUv=aUv;" +
                        "gl_Position=vec4(aPos,0.0,1.0);" +
                        "}";

        private static final String FS =
                "precision mediump float;" +
                        "uniform sampler2D uTex;" +
                        "uniform float uVignette;" +
                        "varying vec2 vUv;" +
                        "float vignette(vec2 uv){" +
                        "float d=distance(uv, vec2(0.5,0.5));" +
                        "return smoothstep(0.75, 0.45, d);" +
                        "}" +
                        "void main(){" +
                        "vec4 o=texture2D(uTex, vUv);" +
                        "float v=vignette(vUv);" +
                        "vec4 dark=vec4(0.0,0.0,0.0,uVignette*(1.0-v));" +
                        "gl_FragColor=o+dark;" +
                        "}";
    }
}

