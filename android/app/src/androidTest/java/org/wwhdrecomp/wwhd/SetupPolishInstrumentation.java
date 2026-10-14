package org.wwhdrecomp.wwhd;

/** Device-side intent check without starting a GPU session or requiring game files. */
public final class SetupPolishInstrumentation extends android.app.Instrumentation {
    @Override public void onCreate(android.os.Bundle arguments) { super.onCreate(arguments); start(); }
    @Override public void onStart() {
        android.os.Bundle result = new android.os.Bundle();
        android.content.Intent intent = SetupService.notificationIntent(getTargetContext());
        if (intent.getComponent() == null ||
                !intent.getComponent().getClassName().equals(SetupActivity.class.getName()) ||
                !intent.getBooleanExtra(SetupActivity.EXTRA_SHOW_SETUP, false)) {
            result.putString("failure", "Notification must bring setup forward, including during shader warming");
            finish(android.app.Activity.RESULT_CANCELED, result);
            return;
        }
        result.putString("result", "Setup notification target passed");
        finish(android.app.Activity.RESULT_OK, result);
    }
}
