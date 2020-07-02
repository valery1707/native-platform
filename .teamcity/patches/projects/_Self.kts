package patches.projects

import jetbrains.buildServer.configs.kotlin.v2019_2.*
import jetbrains.buildServer.configs.kotlin.v2019_2.Project
import jetbrains.buildServer.configs.kotlin.v2019_2.ui.*

/*
This patch script was generated by TeamCity on settings change in UI.
To apply the patch, change the root project
accordingly, and delete the patch script.
*/
changeProject(DslContext.projectId) {
    params {
        add {
            password("env.GRADLE_ENTERPRISE_ACCESS_KEY", "credentialsJSON:42999d0a-3b3d-4d43-9700-a6b542b7bd6f", label = "Gradle Enterprise IAM Access Key", description = "The Gradle Enterprise IAM Access Key used to publish build scans", display = ParameterDisplay.HIDDEN)
        }
    }
}
