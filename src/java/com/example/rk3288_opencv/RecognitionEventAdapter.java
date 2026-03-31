package com.example.rk3288_opencv;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

final class RecognitionEventAdapter extends RecyclerView.Adapter<RecognitionEventAdapter.VH> {
    private final List<RecognitionEvent> items = new ArrayList<>();
    private final SimpleDateFormat sdf = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.CHINA);

    void prepend(@NonNull RecognitionEvent e) {
        items.add(0, e);
        notifyItemInserted(0);
    }

    @NonNull
    @Override
    public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View v = LayoutInflater.from(parent.getContext()).inflate(R.layout.item_recognition_event, parent, false);
        return new VH(v);
    }

    @Override
    public void onBindViewHolder(@NonNull VH holder, int position) {
        holder.bind(items.get(position), sdf);
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    static final class VH extends RecyclerView.ViewHolder {
        private final TextView tvTime;
        private final TextView tvText;

        VH(@NonNull View itemView) {
            super(itemView);
            tvTime = itemView.findViewById(R.id.tv_event_time);
            tvText = itemView.findViewById(R.id.tv_event_text);
        }

        void bind(@NonNull RecognitionEvent e, @NonNull SimpleDateFormat sdf) {
            tvTime.setText(sdf.format(new Date(e.ts)));
            tvText.setText(e.text);
        }
    }
}

