/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)
 */

package emu.skyline

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.content.res.AssetManager
import android.graphics.PointF
import android.net.Uri
import android.os.*
import android.util.Log
import android.view.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.isGone
import androidx.core.view.isInvisible
import emu.skyline.input.*
import emu.skyline.loader.getRomFormat
import emu.skyline.utils.Settings
import kotlinx.android.synthetic.main.emu_activity.*
import java.io.File
import kotlin.math.abs

class EmulationActivity : AppCompatActivity() {
    companion object {
        private val Tag = EmulationActivity::class.java.simpleName
    }


    /**
     * This makes the window fullscreen, sets up the performance statistics and finally calls [executeApplication] for executing the application
     */
    @SuppressLint("SetTextI18n")
    override fun onCreate(savedInstanceState : Bundle?) {
        super.onCreate(savedInstanceState)

        setContentView(R.layout.emu_activity)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.insetsController?.hide(WindowInsets.Type.navigationBars() or WindowInsets.Type.systemBars() or WindowInsets.Type.systemGestures() or WindowInsets.Type.statusBars())
            window.insetsController?.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_FULLSCREEN)
        }


        @Suppress("DEPRECATION") val display = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display!! else windowManager.defaultDisplay
        display?.supportedModes?.maxByOrNull { it.refreshRate + (it.physicalHeight * it.physicalWidth) }?.let { window.attributes.preferredDisplayModeId = it.modeId }

    }

    override fun onDestroy() {
        Log.d(Tag, "OnDestroy called")

        super.onDestroy()
    }


}
