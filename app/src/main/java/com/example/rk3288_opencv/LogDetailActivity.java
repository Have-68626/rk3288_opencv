package com.example.rk3288_opencv;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.MenuItem;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.Toolbar;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class LogDetailActivity extends AppCompatActivity {

    public static final String EXTRA_LOG_PATH = "extra_log_path";
    private static final int MAX_PREVIEW_SIZE = 100 * 1024; // 100 KB

    private TextView tvLogContent;
    private ExecutorService executor = Executors.newSingleThreadExecutor();
    private Handler handler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_log_detail);

        Toolbar toolbar = findViewById(R.id.toolbar_detail);
        setSupportActionBar(toolbar);
        if (getSupportActionBar() != null) {
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
            getSupportActionBar().setDisplayShowHomeEnabled(true);
        }

        tvLogContent = findViewById(R.id.tv_log_content);

        String path = getIntent().getStringExtra(EXTRA_LOG_PATH);
        if (path != null) {
            File file = new File(path);
            getSupportActionBar().setTitle(file.getName());
            loadLogContent(file);
        } else {
            Toast.makeText(this, "File not found", Toast.LENGTH_SHORT).show();
            finish();
        }
    }

    private void loadLogContent(File file) {
        executor.execute(() -> {
            StringBuilder content = new StringBuilder();
            try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
                char[] buffer = new char[1024];
                int totalRead = 0;
                int read;
                while ((read = reader.read(buffer)) != -1) {
                    content.append(buffer, 0, read);
                    totalRead += read;
                    if (totalRead >= MAX_PREVIEW_SIZE) {
                        content.append("\n\n--- PREVIEW TRUNCATED (100KB Limit) ---");
                        break;
                    }
                }
            } catch (IOException e) {
                content.append("Error reading file: ").append(e.getMessage());
            }

            // Desensitize data in preview
            String maskedContent = SensitiveDataUtil.maskSensitiveData(content.toString());

            handler.post(() -> tvLogContent.setText(maskedContent));
        });
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            finish();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }
}
