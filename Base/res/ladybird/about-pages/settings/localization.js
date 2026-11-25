const languageSelect = document.querySelector("#interface-language");

// Only static browser-owned text is translated. User-provided names and site data are left untouched.
const spanish = new Map([
    ["Search settings", "Buscar en la configuración"],
    ["Settings categories", "Categorías de configuración"],
    ["Settings", "Configuración"],
    ["No settings match this search.", "Ningún ajuste coincide con la búsqueda."],
    ["Appearance", "Apariencia"],
    ["Search", "Búsqueda"],
    ["Startup and browsing", "Inicio y navegación"],
    ["Memory", "Memoria"],
    ["Privacy and security", "Privacidad y seguridad"],
    ["Permissions", "Permisos"],
    ["Content blocking", "Bloqueo de contenido"],
    ["Feedback", "Comentarios"],
    ["Advanced", "Avanzado"],
    ["Interface language", "Idioma de la interfaz"],
    ["The settings page updates now. Restart Ladybird to update the browser controls. Some parts are available only in English.", "Esta página se actualiza ahora. Reinicia Ladybird para actualizar los controles del navegador. Algunas partes solo están disponibles en inglés."],
    ["English", "Inglés"],
    ["System", "Sistema"],
    ["Light", "Claro"],
    ["Dark", "Oscuro"],
    ["Accent color", "Color de énfasis"],
    ["The colors of tabs, toolbars, menus and highlights.", "Los colores de las pestañas, barras de herramientas, menús y elementos destacados."],
    ["Websites", "Sitios web"],
    ["Whether websites are asked to show their light or dark design.", "Indica si se solicita a los sitios web que usen su diseño claro u oscuro."],
    ["Force dark mode", "Forzar modo oscuro"],
    ["Darken web pages that don’t provide a dark theme of their own.", "Oscurecer las páginas web que no ofrecen su propio tema oscuro."],
    ["Overrides…", "Excepciones…"],
    ["Layout", "Diseño"],
    ["Tabs on top", "Pestañas arriba"],
    ["Tabs on the side", "Pestañas al lado"],
    ["Everything in the sidebar", "Todo en la barra lateral"],
    ["Expand vertical tabs on hover when collapsed", "Expandir las pestañas verticales al pasar el cursor cuando estén contraídas"],
    ["Sidebar position", "Posición de la barra lateral"],
    ["Left", "Izquierda"],
    ["Right", "Derecha"],
    ["Show system menu bar", "Mostrar la barra de menús del sistema"],
    ["Show bookmarks bar", "Mostrar la barra de marcadores"],
    ["Always", "Siempre"],
    ["Never", "Nunca"],
    ["Only when hovering the navigation toolbar", "Solo al pasar el cursor sobre la barra de navegación"],
    ["Default page zoom", "Zoom predeterminado de las páginas"],
]);

const textEntries = [];
for (const scope of [document.querySelector(".settings-sidebar"), document.querySelector("#tab-appearance"), document.querySelector("#search-empty")]) {
    const walker = document.createTreeWalker(scope, NodeFilter.SHOW_TEXT);
    while (walker.nextNode()) {
        const node = walker.currentNode;
        const source = node.data.trim();
        if (spanish.has(source)) {
            textEntries.push({ node, source: node.data, translation: node.data.replace(source, spanish.get(source)), parent: node.parentElement });
        }
    }
}

const attributeEntries = [];
for (const element of document.querySelectorAll(".settings-sidebar [aria-label], .settings-sidebar [placeholder], #tab-appearance [aria-label], #tab-appearance [title], .tab-panel[data-title]")) {
    for (const name of ["aria-label", "placeholder", "title", "data-title"]) {
        const source = element.getAttribute(name);
        if (spanish.has(source)) attributeEntries.push({ element, name, source });
    }
}

function applyLanguage(language) {
    const useSpanish = language === "es";
    languageSelect.value = useSpanish ? "es" : "en";
    for (const { node, source, translation, parent } of textEntries) {
        node.data = useSpanish ? translation : source;
        if (useSpanish) parent.setAttribute("lang", "es");
        else parent.removeAttribute("lang");
    }
    for (const { element, name, source } of attributeEntries) {
        element.setAttribute(name, useSpanish ? spanish.get(source) : source);
        if (name !== "data-title") {
            if (useSpanish) element.setAttribute("lang", "es");
            else element.removeAttribute("lang");
        }
    }
    document.title = useSpanish ? "Configuración" : "Settings";
}

languageSelect.addEventListener("change", () => {
    ladybird.sendMessage("setConfigVariable", { name: "ui.interface.language", value: languageSelect.value });
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings" && Object.hasOwn(event.detail.data, "configVariables")) {
        applyLanguage(event.detail.data.configVariables?.["ui.interface.language"]);
    }
});
