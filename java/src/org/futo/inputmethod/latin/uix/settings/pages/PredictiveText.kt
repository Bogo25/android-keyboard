package org.futo.inputmethod.latin.uix.settings.pages

import android.content.Intent
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.booleanResource
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.navigation.NavHostController
import androidx.navigation.compose.rememberNavController
import org.futo.inputmethod.dictionarypack.DictionarySettingsActivity
import org.futo.inputmethod.latin.R
import org.futo.inputmethod.latin.settings.Settings
import org.futo.inputmethod.latin.uix.settings.NavigationItem
import org.futo.inputmethod.latin.uix.settings.NavigationItemStyle
import org.futo.inputmethod.latin.uix.settings.ScreenTitle
import org.futo.inputmethod.latin.uix.settings.ScrollableList
import org.futo.inputmethod.latin.uix.settings.SettingToggleSharedPrefs
import org.futo.inputmethod.latin.uix.settings.Tip
import org.futo.inputmethod.latin.uix.settings.useSharedPrefsBool

@Preview
@Composable
fun PredictiveTextScreen(navController: NavHostController = rememberNavController()) {
    val context = LocalContext.current
    ScrollableList {
        ScreenTitle("Predictive Text", showBack = true, navController)

        val (transformerLmEnabled, _) = useSharedPrefsBool(Settings.PREF_KEY_USE_TRANSFORMER_LM, true)


        SettingToggleSharedPrefs(
            title = "Transformer LM",
            key = Settings.PREF_KEY_USE_TRANSFORMER_LM,
            default = true
        )

        if(transformerLmEnabled) {
            NavigationItem(
                title = "Training",
                style = NavigationItemStyle.HomeTertiary,
                navigate = { navController.navigate("trainDev") },
                icon = painterResource(id = R.drawable.cpu)
            )

            Tip("Note: Transformer LM is in alpha state")
        }


        // TODO: It doesn't make a lot of sense in the case of having autocorrect on but show_suggestions off

        SettingToggleSharedPrefs(
            title = stringResource(R.string.auto_correction),
            subtitle = stringResource(R.string.auto_correction_summary),
            key = Settings.PREF_AUTO_CORRECTION,
            default = true
        )
        
        SettingToggleSharedPrefs(
            title = stringResource(R.string.prefs_show_suggestions),
            subtitle = stringResource(R.string.prefs_show_suggestions_summary),
            key = Settings.PREF_SHOW_SUGGESTIONS,
            default = true
        )

        if(!transformerLmEnabled) {
            NavigationItem(
                title = stringResource(R.string.edit_personal_dictionary),
                style = NavigationItemStyle.Misc,
                navigate = {
                    val intent = Intent("android.settings.USER_DICTIONARY_SETTINGS")
                    intent.flags = Intent.FLAG_ACTIVITY_NEW_TASK
                    context.startActivity(intent)
                }
            )

            NavigationItem(
                title = stringResource(R.string.configure_dictionaries_title),
                style = NavigationItemStyle.Misc,
                navigate = {
                    val intent = Intent()
                    intent.flags = Intent.FLAG_ACTIVITY_NEW_TASK
                    intent.setClass(context, DictionarySettingsActivity::class.java)
                    intent.putExtra("clientId", "org.futo.inputmethod.latin")
                    context.startActivity(intent)
                }
            )

            SettingToggleSharedPrefs(
                title = stringResource(R.string.prefs_block_potentially_offensive_title),
                subtitle = stringResource(R.string.prefs_block_potentially_offensive_summary),
                key = Settings.PREF_BLOCK_POTENTIALLY_OFFENSIVE,
                default = booleanResource(R.bool.config_block_potentially_offensive)
            )
        }

        SettingToggleSharedPrefs(
            title = stringResource(R.string.use_personalized_dicts),
            subtitle = stringResource(R.string.use_personalized_dicts_summary),
            key = Settings.PREF_KEY_USE_PERSONALIZED_DICTS,
            default = true
        )

        if(!transformerLmEnabled) {
            SettingToggleSharedPrefs(
                title = stringResource(R.string.bigram_prediction),
                subtitle = stringResource(R.string.bigram_prediction_summary),
                key = Settings.PREF_BIGRAM_PREDICTIONS,
                default = booleanResource(R.bool.config_default_next_word_prediction)
            )
        }
    }
}