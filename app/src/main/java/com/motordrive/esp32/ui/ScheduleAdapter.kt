package com.motordrive.esp32.ui

import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.motordrive.esp32.data.ScheduleEntry
import com.motordrive.esp32.databinding.ItemScheduleBinding

class ScheduleAdapter(
    private val onToggle: (ScheduleEntry, Boolean) -> Unit,
    private val onDelete: (ScheduleEntry) -> Unit
) : ListAdapter<ScheduleEntry, ScheduleAdapter.VH>(DIFF) {

    inner class VH(val b: ItemScheduleBinding) : RecyclerView.ViewHolder(b.root) {
        fun bind(entry: ScheduleEntry) {
            b.tvScheduleTime.text = "${entry.startLabel()} → ${entry.stopLabel()}"
            b.tvScheduleDays.text = entry.daysLabel()
            b.tvAutoRestart.text  = if (entry.autoRestart) "↺ Auto-restart ON" else "↺ Auto-restart OFF"

            // Set checked state without firing listener (prevents push on bind)
            b.switchEnabled.setOnCheckedChangeListener(null)
            b.switchEnabled.isChecked = entry.enabled
            b.switchEnabled.setOnCheckedChangeListener { _, checked -> onToggle(entry, checked) }

            b.btnDelete.setOnClickListener { onDelete(entry) }
        }
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
        VH(ItemScheduleBinding.inflate(LayoutInflater.from(parent.context), parent, false))

    override fun onBindViewHolder(holder: VH, position: Int) = holder.bind(getItem(position))

    companion object {
        private val DIFF = object : DiffUtil.ItemCallback<ScheduleEntry>() {
            override fun areItemsTheSame(a: ScheduleEntry, b: ScheduleEntry) = a.id == b.id
            override fun areContentsTheSame(a: ScheduleEntry, b: ScheduleEntry) = a == b
        }
    }
}
