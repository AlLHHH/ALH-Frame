package com.alh.frame;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * 免责声明文本与同意状态。
 * 首次启动必须同意；之后协议常驻主界面底部，随时可点开重读。
 */
public final class Terms {

    private static final String PREF = "alh_terms";
    private static final String KEY = "agreed_v1";

    public static final String TITLE = "免责声明与使用条款";

    public static final String TEXT =
            "请在使用本应用前完整阅读本条款。点击「同意并继续」即表示你已阅读、理解并接受全部内容；"
          + "不同意请立即退出并停止使用。\n"
        + "\n一、软件性质\n"
          + "本应用为本地视频补帧工具，仅在你自己的设备上运行，不提供任何在线服务，不上传、不收集、"
          + "不传输你的视频与设备数据。\n"
        + "\n二、素材来源与版权\n"
          + "1. 你处理的全部视频素材由你自行提供，来源合法性、授权状态与版权归属由你自行负责。\n"
          + "2. 本应用不提供、不内置、不分发任何影视、音乐或其他受版权保护的内容。\n"
          + "3. 你不得使用本应用处理你无权处理的素材。因素材来源、传播、二次发布引发的任何纠纷、"
          + "投诉、索赔、行政处罚或法律责任，全部由你自行承担。\n"
        + "\n三、内容合法性\n"
          + "你承诺不使用本应用制作、修改、传播违反法律法规或公序良俗的内容。由此产生的一切后果"
          + "由你自行承担。\n"
        + "\n四、结果不担保\n"
          + "1. 补帧结果取决于源素材的画质、码率、运动复杂度与设备性能，本应用不对画质提升幅度、"
          + "帧率准确性、色彩与时长一致性作出任何明示或默示的担保。\n"
          + "2. 补帧会产生插值伪影、边缘抖动、鬼影、局部模糊等现象，属算法固有特性，不视为缺陷。\n"
        + "\n五、风险与责任豁免\n"
          + "1. 使用本应用的一切风险由你自行承担。\n"
          + "2. 在适用法律允许的最大范围内，开发者不对因使用或无法使用本应用而产生的任何直接、"
          + "间接、偶然、特殊、惩罚性或连带损失承担责任，包括但不限于：视频损坏或丢失、画质下降、"
          + "处理中断、设备发热、耗电、卡顿、存储空间被占用、数据丢失、收益损失、商誉损失。\n"
          + "3. 处理过程会占用大量 CPU 与存储空间，长时间满载可能导致设备发热、降频或系统主动"
          + "终止后台任务，由此造成的处理失败与文件不完整，开发者不承担责任。\n"
          + "4. 因设备故障、系统升级、断电、误删、第三方软件干扰等非本应用原因造成的问题，"
          + "开发者不承担责任。\n"
          + "5. 请自行保留源文件备份。本应用不提供数据恢复。\n"
        + "\n六、第三方组件\n"
          + "本应用内置 FFmpeg 命令行程序（GPLv3，含 libx264、libx265、libdav1d 等），以独立进程方式调用，用于视频解码、补帧与编码。其许可证、构建配置与源码获取方式见：\n"
          + "github.com/AlLHHH/ALH-Frame/blob/main/docs/THIRD-PARTY.md\n"
          + "对应源码包（可直接下载）：\n"
          + "github.com/AlLHHH/ALH-Frame/releases/download/v1.0.0/ffmpeg-9.0.2-source.tar.xz\n"
          + "本应用自身代码以 Apache-2.0 许可发布；如需本构建使用的完整源码树，可联系作者索取，三年内有效。\n"
        + "\n七、条款变更\n"
          + "条款可能随版本更新调整，更新后的条款随新版本一同生效。继续使用即视为接受更新后的条款。\n"
        + "\n八、其他\n"
          + "若你不接受本条款的任何部分，请卸载本应用。";

    private Terms() {
    }

    public static boolean agreed(Context c) {
        return prefs(c).getBoolean(KEY, false);
    }

    public static void agree(Context c) {
        prefs(c).edit().putBoolean(KEY, true).apply();
    }

    private static SharedPreferences prefs(Context c) {
        return c.getSharedPreferences(PREF, Context.MODE_PRIVATE);
    }
}