import { localizeAttributes, localizeStaticText, onInterfaceLanguageChange, setInterfaceLanguage, t } from "../l10n.js";

const languageSelect = document.querySelector("#interface-language");

// Only static browser-owned text is translated. User-provided names and site data are left untouched.
localizeStaticText([
    document.querySelector(".settings-sidebar"),
    document.querySelector("#tab-appearance"),
    document.querySelector("#search-empty"),
]);
localizeAttributes(document.querySelectorAll(".tab-panel[data-title]"), ["data-title"]);

onInterfaceLanguageChange(language => {
    languageSelect.value = language;
    document.title = t("Settings");
});

languageSelect.addEventListener("change", () => {
    ladybird.sendMessage("setConfigVariable", { name: "ui.interface.language", value: languageSelect.value });
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings" && Object.hasOwn(event.detail.data, "configVariables")) {
        languageSelect.value = event.detail.data.configVariables?.["ui.interface.language"] === "es" ? "es" : "en";
        setInterfaceLanguage(event.detail.data.configVariables?.["ui.interface.language"], ["chrome", "settings"]);
    }
});
