package org.wwhdrecomp.wwhd;

import android.app.Activity;
import android.app.AlertDialog;
import android.appwidget.AppWidgetManager;
import android.content.Intent;
import android.os.Bundle;

/**
 * The game widget's menu. Only the app icon has launcher shortcuts (export, import, setup); a
 * widget's long-press menu offers its configuration activity instead ("Settings" on Samsung,
 * "Customize" on Pixel), since the widget is reconfigurable. This is that activity: the save
 * tools and setup (a tap on the widget itself plays). Launchers draw that long-press menu
 * themselves; an app can't add items to it for a widget.
 */
public class GameWidgetMenu extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        int id = getIntent().getIntExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, AppWidgetManager.INVALID_APPWIDGET_ID);
        setResult(RESULT_OK, new Intent().putExtra(AppWidgetManager.EXTRA_APPWIDGET_ID, id));
        String[] items = {getString(R.string.save_export), getString(R.string.save_import), getString(R.string.game_setup)};
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(R.string.app_name)
            .setItems(items, (dialog, which) -> {
                Intent next;
                if (which == 0) next = new Intent(this, ExportSaveActivity.class);
                else if (which == 1) next = new Intent(this, ImportSaveActivity.class);
                else next = new Intent(Intent.ACTION_MAIN).setClass(this, SetupActivity.class)
                    .putExtra(SetupActivity.EXTRA_SHOW_SETUP, true);
                startActivity(next.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            })
            .setOnDismissListener(dialog -> finish())
            .show();
    }
}
