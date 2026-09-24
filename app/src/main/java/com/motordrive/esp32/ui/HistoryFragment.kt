package com.motordrive.esp32.ui

import android.os.Bundle
import android.view.View
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.navigation.fragment.findNavController
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.motordrive.esp32.R
import com.motordrive.esp32.databinding.FragmentHistoryBinding
import com.motordrive.esp32.viewmodel.DashboardViewModel
import kotlinx.coroutines.launch

class HistoryFragment : Fragment(R.layout.fragment_history) {

    private var _b: FragmentHistoryBinding? = null
    private val b get() = _b!!
    private val vm: DashboardViewModel by activityViewModels()
    private lateinit var adapter: EventAdapter

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        _b = FragmentHistoryBinding.bind(view)

        ViewCompat.setOnApplyWindowInsetsListener(b.appBarLayout) { v, insets ->
            v.updatePadding(top = insets.getInsets(WindowInsetsCompat.Type.systemBars()).top)
            insets
        }

        b.toolbar.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.action_settings -> {
                    findNavController().navigate(R.id.action_history_to_settings); true
                }
                R.id.action_refresh -> { vm.refresh(); true }
                else -> false
            }
        }

        adapter = EventAdapter()
        b.rvHistory.layoutManager = LinearLayoutManager(requireContext()).apply {
            reverseLayout = true      // newest at top
            stackFromEnd  = false
        }
        b.rvHistory.adapter = adapter

        b.btnClearHistory.setOnClickListener {
            MaterialAlertDialogBuilder(requireContext())
                .setTitle("Clear History")
                .setMessage("Remove all recorded motor events?")
                .setPositiveButton("Clear") { _, _ -> vm.clearHistory() }
                .setNegativeButton("Cancel", null)
                .show()
        }

        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                vm.motorHistory.collect { events ->
                    adapter.submitList(events.reversed())   // show newest first
                    b.tvNoHistory.isVisible     = events.isEmpty()
                    b.rvHistory.isVisible       = events.isNotEmpty()
                    b.btnClearHistory.isEnabled = events.isNotEmpty()

                    val onCount  = events.count { it.motorOn }
                    val offCount = events.count { !it.motorOn }
                    b.tvHistorySummary.text =
                        "${events.size} events  ·  $onCount ON  ·  $offCount OFF"
                }
            }
        }
    }

    override fun onDestroyView() { super.onDestroyView(); _b = null }
}
