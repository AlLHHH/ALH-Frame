package com.alh.frame;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

/**
 * 补帧期间挂一个前台服务。
 * 没有它，用户切到别的应用之后这个过程就是普通后台进程，长时间跑必然被系统回收，
 * 表现是「跑着跑着没了」。有前台通知，进程优先级够高，几十秒到几十分钟的活能跑完。
 */
public class Keep extends Service {

    private static final String CH = "alh_work";
    private static final int ID = 1001;

    static void start(Context c) {
        try {
            Intent i = new Intent(c, Keep.class);
            if (Build.VERSION.SDK_INT >= 26) {
                c.startForegroundService(i);
            } else {
                c.startService(i);
            }
        } catch (Throwable ignored) {
        }
    }

    static void stop(Context c) {
        try {
            c.stopService(new Intent(c, Keep.class));
        } catch (Throwable ignored) {
        }
    }

    public IBinder onBind(Intent i) {
        return null;
    }

    public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            if (Build.VERSION.SDK_INT >= 26 && nm != null) {
                NotificationChannel ch = new NotificationChannel(CH, "补帧任务",
                        NotificationManager.IMPORTANCE_LOW);
                ch.setShowBadge(false);
                nm.createNotificationChannel(ch);
            }
            Notification.Builder b = Build.VERSION.SDK_INT >= 26
                    ? new Notification.Builder(this, CH)
                    : new Notification.Builder(this);
            b.setContentTitle("ALH Frame")
                    .setContentText("正在补帧，完成后会自动保存到相册")
                    .setSmallIcon(android.R.drawable.stat_sys_download)
                    .setOngoing(true);
            startForeground(ID, b.build());
        } catch (Throwable ignored) {
        }
        return START_NOT_STICKY;
    }

    public void onDestroy() {
        try {
            stopForeground(true);
        } catch (Throwable ignored) {
        }
        super.onDestroy();
    }
}