package com.example.rk3288_opencv;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.FrameLayout;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public class LogAdapter extends RecyclerView.Adapter<LogAdapter.LogViewHolder> {

    private List<File> logFiles = new ArrayList<>();
    private Set<File> selectedFiles = new HashSet<>();
    private OnLogClickListener listener;

    public interface OnLogClickListener {
        void onLogClick(File file);
        void onSelectionChanged(int selectedCount, int totalCount);
    }

    public LogAdapter(OnLogClickListener listener) {
        this.listener = listener;
    }

    public void setLogFiles(List<File> files) {
        this.logFiles = new ArrayList<>(files);
        // Sort by Last Modified Descending
        Collections.sort(this.logFiles, new Comparator<File>() {
            @Override
            public int compare(File o1, File o2) {
                return Long.compare(o2.lastModified(), o1.lastModified());
            }
        });
        Set<File> nextSelected = new HashSet<>();
        for (File f : this.logFiles) {
            if (selectedFiles.contains(f)) nextSelected.add(f);
        }
        selectedFiles = nextSelected;
        notifyDataSetChanged();
        notifySelectionChanged();
    }

    public List<File> getSelectedFiles() {
        return new ArrayList<>(selectedFiles);
    }

    public int getSelectedCount() {
        return selectedFiles.size();
    }

    public int getTotalCount() {
        return logFiles.size();
    }

    public boolean hasSelection() {
        return !selectedFiles.isEmpty();
    }
    
    public void selectAll() {
        selectedFiles.addAll(logFiles);
        notifyDataSetChanged();
        notifySelectionChanged();
    }

    public void invertSelection() {
        Set<File> next = new HashSet<>();
        for (File f : logFiles) {
            if (!selectedFiles.contains(f)) {
                next.add(f);
            }
        }
        selectedFiles = next;
        notifyDataSetChanged();
        notifySelectionChanged();
    }
    
    public void clearSelection() {
        selectedFiles.clear();
        notifyDataSetChanged();
        notifySelectionChanged();
    }

    private void notifySelectionChanged() {
        if (listener != null) {
            listener.onSelectionChanged(selectedFiles.size(), logFiles.size());
        }
    }

    private void toggleSelected(File file) {
        if (file == null) return;
        if (selectedFiles.contains(file)) selectedFiles.remove(file);
        else selectedFiles.add(file);
        notifySelectionChanged();
    }

    @NonNull
    @Override
    public LogViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View view = LayoutInflater.from(parent.getContext()).inflate(R.layout.item_log_file, parent, false);
        return new LogViewHolder(view);
    }

    @Override
    public void onBindViewHolder(@NonNull LogViewHolder holder, int position) {
        File file = logFiles.get(position);
        holder.bind(file);
    }

    @Override
    public int getItemCount() {
        return logFiles.size();
    }

    class LogViewHolder extends RecyclerView.ViewHolder {
        TextView tvFilename;
        TextView tvFileInfo;
        CheckBox cbSelect;
        FrameLayout hitSelect;

        public LogViewHolder(@NonNull View itemView) {
            super(itemView);
            tvFilename = itemView.findViewById(R.id.tv_filename);
            tvFileInfo = itemView.findViewById(R.id.tv_file_info);
            cbSelect = itemView.findViewById(R.id.cb_select);
            hitSelect = itemView.findViewById(R.id.hit_select);
        }

        public void bind(final File file) {
            tvFilename.setText(file.getName());
            
            String size = String.format(Locale.US, "%.1f KB", file.length() / 1024.0);
            SimpleDateFormat sdf = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US);
            String date = sdf.format(file.lastModified());
            tvFileInfo.setText(size + " | " + date);

            // Handle Selection
            cbSelect.setOnCheckedChangeListener(null); // Avoid recursion
            cbSelect.setChecked(selectedFiles.contains(file));
            
            cbSelect.setOnCheckedChangeListener((buttonView, isChecked) -> {
                if (isChecked) {
                    selectedFiles.add(file);
                } else {
                    selectedFiles.remove(file);
                }
                notifySelectionChanged();
            });

            if (hitSelect != null) {
                hitSelect.setOnClickListener(v -> cbSelect.toggle());
                hitSelect.setOnLongClickListener(v -> {
                    cbSelect.toggle();
                    return true;
                });
            }

            itemView.setOnLongClickListener(v -> {
                toggleSelected(file);
                int pos = getBindingAdapterPosition();
                if (pos != RecyclerView.NO_POSITION) notifyItemChanged(pos);
                return true;
            });

            itemView.setOnClickListener(v -> {
                if (hasSelection()) {
                    toggleSelected(file);
                    int pos = getBindingAdapterPosition();
                    if (pos != RecyclerView.NO_POSITION) notifyItemChanged(pos);
                    return;
                }
                if (listener != null) {
                    listener.onLogClick(file);
                }
            });
        }
    }
}
