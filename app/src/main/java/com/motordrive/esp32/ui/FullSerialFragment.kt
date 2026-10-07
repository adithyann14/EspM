package com.motordrive.esp32.ui

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Color
import android.os.Bundle
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.View
import android.widget.TextView
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

/**
 * Full-screen ESP sender serial-log viewer.
 * Polls /api/logs every 2 s and colour-codes lines by origin:
 *   [SEND] / [WIFI] / [COMMS] / [SCHED] / [TIMER]  →  cyan  (Sender ESP)
 *   [RECV]                                           →  green (Receiver ESP, forwarded over Wi-Fi)
 *   anything else                                    →  dim grey
 */
class FullSerialFragment : Fragment(R.layout.fragment_full_log) {

    private var _b: FragmentFullLogBinding? = null
    private val b get() = _b!!
    private val vm: DashboardViewModel by activityViewModels()

    // Lines as last fetched from the ESP, and the snapshot taken when Clear was
    // pressed.  The ESP keeps a 30-line ring buffer, so without this the next
    // poll (2 s later) would simply put the cleared lines back.
    private var lastFetched: List<String> = emptyList()
    private var clearBaseline: List<String>? = null
    private var shownLines: List<String> = emptyList()

    // Colour palette for log origins
    private val colSender   = Color.parseColor("#4FC3F7")  // light blue  — Sender
    private val colReceiver = Color.parseColor("#69FF47")  // bright lime — Receiver
    private val colSystem   = Color.parseColor("#888888")  // mid-grey    — unknown / system

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
            clearBaseline = lastFetched
            shownLines    = emptyList()
            b.tvLogContent.text = ""
            b.tvLineCount.text  = "0 lines"
            Toast.makeText(requireContext(), "Cleared", Toast.LENGTH_SHORT).show()
        }
        b.btnCopy.setOnClickListener { copyToClipboard() }

        startPolling()
    }

    override fun onDestroyView() { super.onDestroyView(); _b = null }

    // ── Polling ───────────────────────────────────────────────────────────
    private fun startPolling() {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                while (isActive) {
                    Esp32Repository(vm.config.value).getLogs()
                        .onSuccess { fetched ->
                            if (_b == null) return@onSuccess
                            lastFetched = fetched
                            val base  = clearBaseline
                            val lines = if (base == null) fetched
                                        else fetched.drop(overlap(base, fetched))
                            shownLines = lines
                            if (lines.isEmpty()) {
                                b.tvLogContent.text =
                                    if (base == null) "— waiting for ESP log —" else ""
                                b.tvLineCount.text  = "0 lines"
                            } else {
                                b.tvLogContent.setText(
                                    buildColoredLog(lines),
                                    TextView.BufferType.SPANNABLE
                                )
                                b.tvLineCount.text = "${lines.size} lines"
                                if (b.switchAutoScroll.isChecked) {
                                    b.logScrollView.post {
                                        b.logScrollView.fullScroll(View.FOCUS_DOWN)
                                    }
                                }
                            }
                        }
                        .onFailure { err ->
                            if (_b == null) return@onFailure
                            shownLines = emptyList()
                            b.tvLineCount.text = "offline"
                            b.tvLogContent.text =
                                "⚠ Can't reach the ESP at ${vm.config.value.baseUrl}\n" +
                                "   ${err.message?.take(80)}\n\n" +
                                "• Join the \"MotorControl\" Wi-Fi (password motor1234)\n" +
                                "• If Android says \"no internet\", choose to stay connected\n" +
                                "• Turn off any VPN while using the app"
                        }
                    delay(2_000L)
                }
            }
        }
    }

    /** Longest k such that the last k lines of [old] equal the first k lines of [new]. */
    private fun overlap(old: List<String>, new: List<String>): Int {
        val max = minOf(old.size, new.size)
        for (k in max downTo 1) {
            if (old.subList(old.size - k, old.size) == new.subList(0, k)) return k
        }
        return 0
    }

    // ── Colour-coded log builder ──────────────────────────────────────────
    private fun buildColoredLog(lines: List<String>): SpannableStringBuilder {
        val sb = SpannableStringBuilder()

        // Sticky legend line
        val legend = "● Sender  ● Receiver\n"
        val legendStart = 0
        sb.append(legend)
        // "● Sender"
        sb.setSpan(ForegroundColorSpan(colSender),   0, 8,
                   Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        // "● Receiver"
        sb.setSpan(ForegroundColorSpan(colReceiver), 10, 20,
                   Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        // rest of legend in grey
        sb.setSpan(ForegroundColorSpan(colSystem),   legendStart, legend.length,
                   Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        // Re-colour the two bullets correctly (set after grey to override)
        sb.setSpan(ForegroundColorSpan(colSender),   0, 8,
                   Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        sb.setSpan(ForegroundColorSpan(colReceiver), 10, 20,
                   Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)

        for (line in lines) {
            val start = sb.length
            sb.append(line)
            sb.append('\n')
            val color = lineColor(line)
            sb.setSpan(ForegroundColorSpan(color), start, sb.length,
                       Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        return sb
    }

    private fun lineColor(line: String): Int = when {
        line.contains("[SEND]") || line.contains("[WIFI]") ||
        line.contains("[COMMS]") || line.contains("[SCHED]") ||
        line.contains("[TIMER]")  -> colSender
        line.contains("[RECV]")   -> colReceiver
        else                      -> colSystem
    }

    // ── Clipboard ─────────────────────────────────────────────────────────
    private fun copyToClipboard() {
        // Copy the real log lines (not the legend / error text shown on screen)
        val text = shownLines.joinToString("\n")
        if (text.isBlank()) {
            Toast.makeText(requireContext(), "Nothing to copy", Toast.LENGTH_SHORT).show()
            return
        }
        val cm = requireContext().getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("ESP Serial Log", text))
        Toast.makeText(requireContext(), "Copied", Toast.LENGTH_SHORT).show()
    }
}
