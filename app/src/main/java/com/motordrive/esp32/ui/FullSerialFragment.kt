package com.motordrive.esp32.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Bundle
import android.view.View
import android.widget.Toast
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.navigation.fragment.findNavController
import com.motordrive.esp32.R
import com.motordrive.esp32.data.Esp32Repository
import com.motordrive.esp32.databinding.FragmentFullLogBinding
import com.motordrive.esp32.viewmodel.DashboardViewModel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/** Full-screen ESP sender serial-log viewer. Polls /api/logs every 2 s. */
class FullSerialFragment : Fragment(R.layout.fragment_full_log) {

    private var _b: FragmentFullLogBinding? = null
    private val b get() = _b!!
    private val vm: DashboardViewModel by activityViewModels()

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        _b = FragmentFullLogBinding.bind(view)

        ViewCompat.setOnApplyWindowInsetsListener(b.appBarLayout) { v, insets ->
            v.updatePadding(top = insets.getInsets(WindowInsetsCompat.Type.systemBars()).top)
            insets
        }
        b.toolbar.title = "Serial Monitor"
        b.toolbar.setNavigationOnClickListener { findNavController().navigateUp() }

        b.btnClearLog.setOnClickListener {
            b.tvLogContent.text = ""
            b.tvLineCount.text  = "0 lines"
        }
        b.btnCopy.setOnClickListener { copyToClipboard() }

        startPolling()
    }

    override fun onDestroyView() { super.onDestroyView(); _b = null }

    private fun startPolling() {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                while (isActive) {
                    Esp32Repository(vm.config.value).getLogs()
                        .onSuccess { lines ->
                            if (_b == null) return@onSuccess
                            val text = lines.joinToString("\n")
                            b.tvLogContent.text = text
                            b.tvLineCount.text  = "${lines.size} lines"
                            if (b.switchAutoScroll.isChecked && text.isNotEmpty()) {
                                b.logScrollView.post {
                                    b.logScrollView.fullScroll(View.FOCUS_DOWN)
                                }
                            }
                        }
                        .onFailure {
                            if (_b != null)
                                b.tvLineCount.text = "⚠ ${it.message?.take(40)}"
                        }
                    delay(2_000L)
                }
            }
        }
    }

    private fun copyToClipboard() {
        val text = b.tvLogContent.text?.toString() ?: return
        if (text.isBlank()) { Toast.makeText(requireContext(), "Nothing to copy", Toast.LENGTH_SHORT).show(); return }
        val cm = requireContext().getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("ESP Serial Log", text))
        Toast.makeText(requireContext(), "Copied", Toast.LENGTH_SHORT).show()
    }
}
