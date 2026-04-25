import re

with open('.github/workflows/ci.yml', 'r') as f:
    content = f.read()

# The error states:
# Exception in thread "main" java.io.IOException: Downloading from https://services.gradle.org/distributions/gradle-9.0-milestone-1-bin.zip failed: timeout (10000ms)
#
# Let's adjust Gradle timeout properties to prevent this. We can inject an environment variable or create a gradle.properties file.

content = content.replace(
    """      - name: Build and Test (Skip OpenCV)
        run: ./gradlew -PRK_SKIP_OPENCV=true :app:assembleDebug :app:testDebugUnitTest :app:lintDebug""",
    """      - name: Configure Gradle timeouts
        run: |
          mkdir -p ~/.gradle
          echo "systemProp.org.gradle.internal.http.connectionTimeout=60000" >> ~/.gradle/gradle.properties
          echo "systemProp.org.gradle.internal.http.socketTimeout=60000" >> ~/.gradle/gradle.properties
      - name: Build and Test (Skip OpenCV)
        run: ./gradlew -PRK_SKIP_OPENCV=true :app:assembleDebug :app:testDebugUnitTest :app:lintDebug --no-daemon"""
)

with open('.github/workflows/ci.yml', 'w') as f:
    f.write(content)
