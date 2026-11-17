package org.t2dos.term

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper

/**
 * Keeps the app running while a session is open (a foreground service, shown as a notification
 * with a Disconnect button), so switching to another app does not end it.  It holds no session
 * itself: [Sessions] does, and tells it through onActive when one opens or ends.
 */
class SessionService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() { super.onCreate(); instance = this }
    override fun onDestroy() { instance = null; super.onDestroy() }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // always go to the foreground first: Android requires it after startForegroundService
        val n = notification()
        if (Build.VERSION.SDK_INT >= 34) startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)
        else startForeground(ID, n)
        if (intent?.action == ACTION_DISCONNECT) Sessions.disconnect()
        sync()
        return START_NOT_STICKY
    }

    /** Stops when no session is open any more. */
    private fun sync() {
        val c = Sessions.current
        if (c == null || c.closedReason != null) { stopForeground(STOP_FOREGROUND_REMOVE); stopSelf() }
    }

    private fun notification(): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        if (nm.getNotificationChannel(CHANNEL) == null)
            nm.createNotificationChannel(NotificationChannel(CHANNEL, "Open session", NotificationManager.IMPORTANCE_LOW))
        val board = Sessions.current?.board
        val open = PendingIntent.getActivity(this, 0,
            Intent(this, TerminalActivity::class.java).putExtra(TerminalActivity.EXTRA_BOARD, board?.id ?: -1L)
                .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val stop = PendingIntent.getService(this, 1, Intent(this, SessionService::class.java).setAction(ACTION_DISCONNECT),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_prompt)
            .setContentTitle("Connected to ${board?.name ?: "a board"}")
            .setContentText(board?.let { "${it.host}:${it.port}" } ?: "")
            .setContentIntent(open)
            .setOngoing(true)
            .addAction(Notification.Action.Builder(null, "Disconnect", stop).build())
            .build()
    }

    companion object {
        private const val ID = 1
        private const val CHANNEL = "session"
        private const val ACTION_DISCONNECT = "org.t2dos.term.DISCONNECT"
        @Volatile private var instance: SessionService? = null
        private val main = Handler(Looper.getMainLooper())

        /** Shows the notification again for the open session (after the permission came, or on return). */
        fun refresh(ctx: Context) {
            val c = Sessions.current ?: return
            if (c.closedReason == null) ctx.applicationContext.startForegroundService(Intent(ctx.applicationContext, SessionService::class.java))
        }

        /** Connects Sessions to the service (once per process). */
        fun install(ctx: Context) {
            val app = ctx.applicationContext
            Sessions.onActive = { on ->
                main.post {
                    if (on) app.startForegroundService(Intent(app, SessionService::class.java))
                    else instance?.sync()
                }
            }
        }
    }
}
