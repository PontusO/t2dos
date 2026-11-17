plugins {
    id("com.android.application")
}

android {
    namespace = "org.t2dos.term"
    compileSdk = 36
    defaultConfig {
        applicationId = "org.t2dos.term"
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"
    }
    testOptions {
        unitTests.all { test ->
            // the end-to-end test reads these (tools/check_android.py sets them)
            listOf("T2DOS_E2E_PORT", "T2DOS_E2E_PASS").forEach { k -> System.getenv(k)?.let { test.environment(k, it) } }
            test.testLogging { events("passed", "skipped", "failed"); showStandardStreams = false }
        }
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
