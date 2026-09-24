package com.motordrive.esp32.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.Toast
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.Fragment
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.navigation.fragment.findNavController
import com.motordrive.esp32.AppLogger
import com.motordrive.esp32.R
import com.motordrive.esp32.databinding.FragmentFullLogBinding
import kotlinx.coroutines.launch

/** Full-screen in-app logcat viewer. Observes AppLogger.flow live. */
class FullLogcatFragment : Fragment(R.layout.fragment_full_log) {

    private var _b: FragmentFullLogBinding? = null
    private val b get() = _b!!

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        _b = FragmentFullLogBinding.bind(view)

        ViewCompat.setOnApplyWindowInsetsListener(b.appBarLayout) { v, insets ->
            v.updatePadding(top = insets.getInsets(WindowInsetsCompat.Type.systemBars()).top)
            insets
        }
        b.toolbar.title = "App Logcat"
        b.toolbar.setNavigationOnClickListener { findNavController().navigateUp() }

        b.btnClearLog.setOnClickListener {
            AppLogger.clear()
            Toast.makeText(requireContext(), "Log cleared", Toast.LENGTH_SHORT).show()
        }

        b.btnCopy.setOnClickListener {
            val text = b.tvLogContent.text?.toString() ?: ""
            if (text.isBlank()) {
                Toast.makeText(requireContext(), "Nothing to copy", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            val cm = requireContext().getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("App Log", text))
            Toast.makeText(requireContext(), "Copied", Toast.LENGTH_SHORT).show()
        }

        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                AppLogger.flow.collect { entries ->
                    if (_b == null) return@collect
                    val text = entries.joinToString("\n") { it.format() }
                    b.tvLogContent.text = text
                    b.tvLineCount.text  = "${entries.size} lines"
                    if (b.switchAutoScroll.isChecked && text.isNotEmpty()) {
                        b.logScrollView.post { b.logScrollView.fullScroll(View.FOCUS_DOWN) }
                    }
                }
            }
        }
    }

    override fun onDestroyView() { super.onDestroyView(); _b = null }
}
