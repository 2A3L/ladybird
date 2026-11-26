// Search ignores case and accents, so "configuracion" finds "Configuración".
export function normalizeSearchText(text) {
    return text.normalize("NFD").replace(/\p{M}/gu, "").toLocaleLowerCase();
}
