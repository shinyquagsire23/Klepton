if (Test-Path ./log.log)
{
    & del log.log
}
& adb logcat -c
& adb logcat > log.log