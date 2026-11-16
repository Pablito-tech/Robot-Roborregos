# Buscador de ofertas Amazon México (Buen Fin)

Script de Python que busca productos en **amazon.com.mx**, calcula el **descuento real** de cada uno (precio actual contra precio tachado) y te arma una lista ordenada con las mejores ofertas, filtrando por calificación, reseñas, precio y más.

## Instalación (una vez)

Necesitas Python 3.10 o más nuevo.

```bash
cd ofertas-amazon
python -m venv .venv
source .venv/bin/activate        # En Windows: .venv\Scripts\activate
pip install -r requirements.txt
```

## Uso

```bash
# Una o varias búsquedas
python buscar_ofertas.py "audifonos bluetooth" "freidora de aire" --min-descuento 30

# Usar la lista de busquedas.txt, solo productos con 4+ estrellas y 100+ reseñas, y abrir el reporte
python buscar_ofertas.py -a busquedas.txt -d 25 -e 4 -r 100 --sin-patrocinados --abrir

# Con presupuesto
python buscar_ofertas.py "pantalla 55 pulgadas" --precio-max 9000 -e 4.2
```

Al terminar verás una tabla en la terminal y se generan dos archivos:

- `ofertas.html`: reporte con fotos, precios y links (ábrelo en el navegador o usa `--abrir`).
- `ofertas.csv`: para abrir en Excel o Google Sheets.

### Opciones principales

| Opción | Qué hace |
|---|---|
| `-a busquedas.txt` | Lee las búsquedas de un archivo (una por línea) |
| `-d 30` | Descuento mínimo en % (por defecto 20) |
| `-e 4` | Calificación mínima |
| `-r 100` | Reseñas mínimas |
| `--precio-min` / `--precio-max` | Rango de precio en pesos |
| `--excluir funda mica` | Quita productos cuyo título tenga esas palabras |
| `--sin-patrocinados` | Oculta anuncios |
| `--solo-prime` | Solo productos con Prime |
| `-p 3` | Páginas de resultados por búsqueda (por defecto 2) |
| `-o descuento` | Ordenar por `puntaje` (por defecto), `descuento`, `ahorro`, `precio` o `estrellas` |
| `--navegador` | Usa un Chrome real si Amazon bloquea el modo normal |

Ver todas: `python buscar_ofertas.py -h`

### ¿Qué es el "puntaje"?

Un descuento de 80% en un producto con 7 reseñas de 2.8 estrellas casi nunca es buena compra. El puntaje combina:

`descuento × (estrellas / 5)² × confianza por reseñas`

La confianza va de 0.3 (sin reseñas) a 1.0 (1000 reseñas o más). Así suben los productos con buen descuento **y** buena reputación.

## Si Amazon te bloquea

Amazon a veces responde con un captcha a programas automáticos. Si ves "Amazon bloqueó la petición":

1. Espera unos minutos y sube la espera: `--espera 6`.
2. Usa el modo navegador, que abre un Chrome de verdad (si sale captcha lo resuelves tú en la ventana):

   ```bash
   pip install playwright
   playwright install chromium
   python buscar_ofertas.py -a busquedas.txt --navegador
   ```

## Consejos para el Buen Fin

- **Revisa el historial de precio.** Algunas tiendas suben el "precio antes" días previos para inflar el descuento. Cada producto del reporte HTML tiene un link a **Keepa** con su historial en Amazon México.
- Revisa si hay **cupón** (aparece marcado): se aplica encima del precio mostrado.
- Mira **quién vende**: en la página del producto, mejor "Vendido por Amazon" o vendedores con buena calificación.
- Compara con meses sin intereses de tu banco: a veces cambian el precio final.

## Notas

- El programa lee las páginas públicas de búsqueda de Amazon, igual que un navegador. Es para uso personal y moderado: no le bajes mucho a `--espera`. Los términos de Amazon no permiten la extracción automatizada a gran escala.
- Si Amazon cambia el diseño de su página, el lector puede dejar de encontrar precios. Con `--guardar-html-crudo` se guarda el HTML recibido para poder ajustar los selectores en `parse_resultados()`.

## Pruebas

```bash
pip install pytest
python -m pytest tests
```
