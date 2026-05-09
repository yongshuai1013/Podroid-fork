/*
 * Podroid - Rootless Podman for Android
 * Copyright (C) 2024-2026 Podroid contributors
 */
package com.excp.podroid.ui.screens.x11

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.excp.podroid.engine.PodroidQemu
import com.excp.podroid.engine.VmState
import com.excp.podroid.x11.AudioStreamer
import com.excp.podroid.x11.VncClient
import com.excp.podroid.x11.X11Constants
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import javax.inject.Inject

sealed interface X11ConnectionState {
    object Disconnected : X11ConnectionState
    object Connecting : X11ConnectionState
    object Connected : X11ConnectionState
    data class Failed(val message: String) : X11ConnectionState
}

@HiltViewModel
class X11ViewModel @Inject constructor(
    val podroidQemu: PodroidQemu,
) : ViewModel() {

    val vmState: StateFlow<VmState> = podroidQemu.state

    private val _connection = MutableStateFlow<X11ConnectionState>(X11ConnectionState.Disconnected)
    val connection: StateFlow<X11ConnectionState> = _connection.asStateFlow()

    /** Backing pixel buffer the SurfaceView blits to a Bitmap. */
    val framebuffer: IntArray = IntArray(X11Constants.FB_WIDTH * X11Constants.FB_HEIGHT)

    private val _frameCounter = MutableStateFlow(0)
    val frameCounter: StateFlow<Int> = _frameCounter.asStateFlow()

    private val audio = AudioStreamer()
    private var sessionJob: Job? = null
    private var rfbOut: OutputStream? = null

    fun connect() {
        if (sessionJob?.isActive == true) return
        _connection.value = X11ConnectionState.Connecting
        sessionJob = viewModelScope.launch(Dispatchers.IO) {
            try {
                Socket().use { sock ->
                    sock.connect(InetSocketAddress("127.0.0.1", X11Constants.VNC_PORT), 2000)
                    val inp = sock.getInputStream()
                    val out = sock.getOutputStream()
                    rfbOut = out

                    VncClient.handshake(inp, out)
                    VncClient.negotiatePixelFormat(out)
                    VncClient.requestFramebufferUpdate(out, incremental = false)

                    _connection.value = X11ConnectionState.Connected
                    audio.start(viewModelScope)

                    while (isActive) {
                        VncClient.readFramebufferUpdate(
                            inp = inp,
                            targetArgb = framebuffer,
                            stride = X11Constants.FB_WIDTH,
                        )
                        _frameCounter.value = _frameCounter.value + 1
                        VncClient.requestFramebufferUpdate(out, incremental = true)
                    }
                }
            } catch (e: Exception) {
                _connection.value = X11ConnectionState.Failed(e.message ?: "unknown")
            } finally {
                rfbOut = null
                audio.stop()
                if (_connection.value !is X11ConnectionState.Failed) {
                    _connection.value = X11ConnectionState.Disconnected
                }
            }
        }
    }

    fun disconnect() {
        sessionJob?.cancel()
        sessionJob = null
    }

    fun sendPointer(x: Int, y: Int, buttonMask: Int) {
        val out = rfbOut ?: return
        viewModelScope.launch(Dispatchers.IO) {
            runCatching { VncClient.sendPointer(out, x, y, buttonMask) }
        }
    }

    fun sendKey(keysym: Int, down: Boolean) {
        val out = rfbOut ?: return
        viewModelScope.launch(Dispatchers.IO) {
            runCatching { VncClient.sendKey(out, keysym, down) }
        }
    }

    override fun onCleared() {
        disconnect()
        super.onCleared()
    }
}
