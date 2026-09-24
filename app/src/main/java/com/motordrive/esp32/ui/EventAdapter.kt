package com.motordrive.esp32.ui

import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.motordrive.esp32.R
import com.motordrive.esp32.data.MotorEvent
import com.motordrive.esp32.databinding.ItemMotorEventBinding

class EventAdapter : ListAdapter<MotorEvent, EventAdapter.VH>(DIFF) {

    inner class VH(val b: ItemMotorEventBinding) : RecyclerView.ViewHolder(b.root) {
        fun bind(e: MotorEvent) {
            val ctx = b.root.context
            if (e.motorOn) {
                b.tvEventState.text = "● MOTOR ON"
                b.tvEventState.setTextColor(ctx.getColor(R.color.motor_on))
                b.eventDot.setBackgroundColor(ctx.getColor(R.color.motor_on))
            } else {
                b.tvEventState.text = "○ MOTOR OFF"
                b.tvEventState.setTextColor(ctx.getColor(R.color.motor_off))
                b.eventDot.setBackgroundColor(ctx.getColor(R.color.motor_off))
            }
            b.tvEventTrigger.text = e.triggerLabel()
            b.tvEventTime.text    = e.timeLabel()
        }
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
        VH(ItemMotorEventBinding.inflate(LayoutInflater.from(parent.context), parent, false))

    override fun onBindViewHolder(holder: VH, position: Int) =
        holder.bind(getItem(position))

    companion object {
        private val DIFF = object : DiffUtil.ItemCallback<MotorEvent>() {
            override fun areItemsTheSame(a: MotorEvent, b: MotorEvent) = a.id == b.id
            override fun areContentsTheSame(a: MotorEvent, b: MotorEvent) = a == b
        }
    }
}
