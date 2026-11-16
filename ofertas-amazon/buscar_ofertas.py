#!/usr/bin/env python3
"""
Buscador de ofertas en Amazon México (pensado para el Buen Fin).

Busca una o varias palabras clave en amazon.com.mx, lee los resultados,
calcula el descuento real de cada producto (precio actual vs. precio tachado)
y te deja filtrar y ordenar para encontrar las mejores ofertas rápido.

Uso rápido:
    python buscar_ofertas.py "audifonos bluetooth" "freidora de aire" --min-descuento 30
    python buscar_ofertas.py -a busquedas.txt --min-descuento 25 --min-estrellas 4 --abrir

Genera:
    - una tabla en la terminal
    - ofertas.csv (para abrir en Excel / Sheets)
    - ofertas.html (reporte con fotos, links y link a Keepa para ver el historial de precio)
"""

from __future__ import annotations

import argparse
import csv
import html
import math
import random
import re
import sys
import time
import webbrowser
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path
from urllib.parse import quote_plus

BASE = "https://www.amazon.com.mx"

USER_AGENTS = [
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/130.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/130.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:132.0) Gecko/20100101 Firefox/132.0",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) "
    "Version/17.6 Safari/605.1.15",
]

ORDEN_AMAZON = {
    "relevancia": None,
    "precio-asc": "price-asc-rank",
    "precio-desc": "price-desc-rank",
    "reseñas": "review-rank",
    "nuevos": "date-desc-rank",
}


# --------------------------------------------------------------------------- #
# Modelo
# --------------------------------------------------------------------------- #
@dataclass
class Producto:
    asin: str
    titulo: str
    precio: float | None
    precio_lista: float | None
    estrellas: float | None
    resenas: int | None
    url: str
    imagen: str = ""
    cupon: str = ""
    prime: bool = False
    patrocinado: bool = False
    busqueda: str = ""
    busquedas: list[str] = field(default_factory=list)

    @property
    def descuento(self) -> float:
        """Porcentaje de descuento (0 si no hay precio tachado)."""
        if self.precio and self.precio_lista and self.precio_lista > self.precio:
            return round((1 - self.precio / self.precio_lista) * 100, 1)
        return 0.0

    @property
    def ahorro(self) -> float:
        if self.precio and self.precio_lista and self.precio_lista > self.precio:
            return round(self.precio_lista - self.precio, 2)
        return 0.0

    @property
    def puntaje(self) -> float:
        """
        Qué tan buena es la oferta combinando descuento y confianza en el producto.
        - Descuento alto suma.
        - Pocas estrellas o pocas reseñas restan (un 70% de descuento en un producto
          con 3 reseñas de 2.5 estrellas no es buena oferta).
        """
        d = self.descuento
        if d <= 0:
            return 0.0
        est = self.estrellas if self.estrellas is not None else 3.5
        res = self.resenas if self.resenas is not None else 0
        factor_estrellas = (est / 5) ** 2
        factor_resenas = min(1.0, math.log10(res + 1) / 3)  # 1000+ reseñas = confianza total
        factor_resenas = 0.3 + 0.7 * factor_resenas
        return round(d * factor_estrellas * factor_resenas, 1)

    @property
    def keepa(self) -> str:
        # Keepa: dominio 11 = amazon.com.mx. Sirve para ver si el "precio antes" es real.
        return f"https://keepa.com/#!product/11-{self.asin}"


# --------------------------------------------------------------------------- #
# Parseo
# --------------------------------------------------------------------------- #
def parse_precio(texto: str | None) -> float | None:
    """'$1,299.00' -> 1299.0 ; 'MXN 499' -> 499.0"""
    if not texto:
        return None
    limpio = re.sub(r"[^\d.,]", "", texto)
    if not limpio:
        return None
    # Formato México: coma = miles, punto = decimales.
    limpio = limpio.replace(",", "")
    try:
        return float(limpio)
    except ValueError:
        return None


def parse_estrellas(texto: str | None) -> float | None:
    """'4.5 de 5 estrellas' / '4,5 de 5' / '4.5 out of 5 stars' -> 4.5"""
    if not texto:
        return None
    m = re.search(r"(\d+[.,]?\d*)\s*(?:de|out of)\s*5", texto)
    if not m:
        return None
    try:
        return float(m.group(1).replace(",", "."))
    except ValueError:
        return None


def parse_resenas(texto: str | None) -> int | None:
    """'1,234' -> 1234 ; '(2.5 mil)' -> 2500 ; '12K' -> 12000"""
    if not texto:
        return None
    t = texto.lower().strip().strip("()").strip()
    m = re.search(r"(\d+(?:[.,]\d+)*)\s*(mil|k)?", t)
    if not m:
        return None
    num, mult = m.group(1), m.group(2)
    if mult:
        return int(float(num.replace(",", ".")) * 1000)
    return int(num.replace(",", "").replace(".", ""))


def es_captcha(html_txt: str) -> bool:
    t = html_txt.lower()
    return (
        "validatecaptcha" in t
        or "introduce los caracteres" in t
        or "escriba los caracteres" in t
        or "type the characters you see" in t
        or "api-services-support@amazon.com" in t
    )


def parse_resultados(html_txt: str, busqueda: str = "") -> list[Producto]:
    from bs4 import BeautifulSoup

    soup = BeautifulSoup(html_txt, "html.parser")
    productos: list[Producto] = []

    for card in soup.select('div[data-component-type="s-search-result"]'):
        asin = (card.get("data-asin") or "").strip()
        if not asin:
            continue

        h2 = card.find("h2")
        titulo = ""
        if h2:
            titulo = (h2.get("aria-label") or h2.get_text(" ", strip=True)).strip()
        titulo = re.sub(r"^(Anuncio patrocinado\s*[-–:]\s*|Patrocinado\s*[-–:]?\s*)", "", titulo)
        if not titulo:
            continue

        precio_el = card.select_one("span.a-price:not(.a-text-price) span.a-offscreen")
        lista_el = card.select_one("span.a-price.a-text-price span.a-offscreen")
        precio = parse_precio(precio_el.get_text() if precio_el else None)
        precio_lista = parse_precio(lista_el.get_text() if lista_el else None)

        est_el = card.select_one("span.a-icon-alt")
        estrellas = parse_estrellas(est_el.get_text() if est_el else None)

        resenas = None
        res_el = card.select_one('[aria-label$="calificaciones"], [aria-label$="ratings"]')
        if res_el is not None:
            resenas = parse_resenas(res_el.get("aria-label"))
        if resenas is None:
            res_el = card.select_one("span.a-size-base.s-underline-text, span.a-size-mini.s-underline-text")
            if res_el is not None:
                resenas = parse_resenas(res_el.get_text())

        img_el = card.select_one("img.s-image")
        imagen = img_el.get("src", "") if img_el else ""

        cupon = ""
        for el in card.select(".s-coupon-unclipped, .s-coupon-clipped, span"):
            txt = el.get_text(" ", strip=True)
            if "cupón" in txt.lower() and len(txt) < 80:
                cupon = txt
                break

        prime = card.select_one("i.a-icon-prime") is not None
        texto_card = card.get_text(" ", strip=True)
        patrocinado = (
            card.select_one(".puis-sponsored-label-text, .s-sponsored-label-text") is not None
            or texto_card.startswith("Patrocinado")
        )

        productos.append(
            Producto(
                asin=asin,
                titulo=titulo,
                precio=precio,
                precio_lista=precio_lista,
                estrellas=estrellas,
                resenas=resenas,
                url=f"{BASE}/dp/{asin}",
                imagen=imagen,
                cupon=cupon,
                prime=prime,
                patrocinado=patrocinado,
                busqueda=busqueda,
            )
        )
    return productos


# --------------------------------------------------------------------------- #
# Descarga
# --------------------------------------------------------------------------- #
def url_busqueda(palabras: str, pagina: int, orden: str | None, precio_min=None, precio_max=None) -> str:
    url = f"{BASE}/s?k={quote_plus(palabras)}&page={pagina}"
    s = ORDEN_AMAZON.get(orden or "relevancia")
    if s:
        url += f"&s={s}"
    if precio_min is not None or precio_max is not None:
        # Pista para Amazon; el filtro que cuenta lo aplicamos nosotros después.
        if precio_min is not None:
            url += f"&low-price={int(precio_min)}"
        if precio_max is not None:
            url += f"&high-price={int(precio_max)}"
    return url


class DescargadorRequests:
    def __init__(self, espera: float):
        import requests

        self.espera = espera
        self.s = requests.Session()
        self.s.headers.update(
            {
                "User-Agent": random.choice(USER_AGENTS),
                "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
                "Accept-Language": "es-MX,es;q=0.9,en;q=0.6",
                "Connection": "keep-alive",
                "Upgrade-Insecure-Requests": "1",
            }
        )
        self._calentado = False

    def obtener(self, url: str) -> str | None:
        if not self._calentado:
            # Visitar la portada primero para obtener cookies, como un navegador normal.
            try:
                self.s.get(BASE, timeout=20)
            except Exception:
                pass
            self._calentado = True
            time.sleep(self.espera)

        for intento in range(3):
            try:
                r = self.s.get(url, timeout=25)
            except Exception as e:
                print(f"   ! error de red: {e}", file=sys.stderr)
                time.sleep(self.espera * (intento + 1))
                continue
            if r.status_code == 200 and not es_captcha(r.text):
                return r.text
            motivo = "captcha" if es_captcha(r.text) else f"HTTP {r.status_code}"
            print(f"   ! Amazon respondió con {motivo}; reintentando ({intento + 1}/3)...", file=sys.stderr)
            self.s.headers["User-Agent"] = random.choice(USER_AGENTS)
            time.sleep(self.espera * (intento + 2) + random.uniform(1, 3))
        return None

    def cerrar(self):
        self.s.close()


class DescargadorNavegador:
    """Usa un Chrome real (Playwright). Más lento pero Amazon casi no lo bloquea,
    y si sale captcha lo puedes resolver tú en la ventana."""

    def __init__(self, espera: float, oculto: bool):
        try:
            from playwright.sync_api import sync_playwright
        except ImportError:
            sys.exit(
                "Para --navegador necesitas Playwright:\n"
                "    pip install playwright\n"
                "    playwright install chromium"
            )
        self.espera = espera
        self._pw = sync_playwright().start()
        self._browser = self._pw.chromium.launch(headless=oculto)
        self._ctx = self._browser.new_context(locale="es-MX", user_agent=USER_AGENTS[0])
        self.page = self._ctx.new_page()

    def obtener(self, url: str) -> str | None:
        try:
            self.page.goto(url, wait_until="domcontentloaded", timeout=45000)
        except Exception as e:
            print(f"   ! error cargando página: {e}", file=sys.stderr)
            return None
        contenido = self.page.content()
        if es_captcha(contenido):
            print("   >> Amazon pidió captcha. Resuélvelo en la ventana del navegador (tienes 2 min)...")
            try:
                self.page.wait_for_selector('div[data-component-type="s-search-result"]', timeout=120000)
            except Exception:
                return None
            contenido = self.page.content()
        try:
            self.page.wait_for_selector('div[data-component-type="s-search-result"]', timeout=10000)
            contenido = self.page.content()
        except Exception:
            pass
        return contenido

    def cerrar(self):
        self._browser.close()
        self._pw.stop()


# --------------------------------------------------------------------------- #
# Filtros, orden y salida
# --------------------------------------------------------------------------- #
def filtrar(productos: list[Producto], a: argparse.Namespace) -> list[Producto]:
    excluir = [w.lower() for w in (a.excluir or [])]
    out = []
    for p in productos:
        if p.precio is None:
            continue
        if p.descuento < a.min_descuento:
            continue
        if a.precio_max is not None and p.precio > a.precio_max:
            continue
        if a.precio_min is not None and p.precio < a.precio_min:
            continue
        if a.min_estrellas and (p.estrellas is None or p.estrellas < a.min_estrellas):
            continue
        if a.min_resenas and (p.resenas is None or p.resenas < a.min_resenas):
            continue
        if a.sin_patrocinados and p.patrocinado:
            continue
        if a.solo_prime and not p.prime:
            continue
        if excluir and any(w in p.titulo.lower() for w in excluir):
            continue
        out.append(p)
    return out


CLAVES_ORDEN = {
    "puntaje": lambda p: -p.puntaje,
    "descuento": lambda p: -p.descuento,
    "ahorro": lambda p: -p.ahorro,
    "precio": lambda p: p.precio or 0,
    "estrellas": lambda p: -(p.estrellas or 0),
}


def dinero(x: float | None) -> str:
    return "—" if x is None else f"${x:,.2f}"


def imprimir_tabla(productos: list[Producto], limite: int):
    if not productos:
        print("\nNo encontré ofertas con esos filtros. Prueba bajar --min-descuento o --min-estrellas.")
        return
    print()
    print(f"{'#':>3}  {'Desc':>5}  {'Precio':>11}  {'Antes':>11}  {'★':>3}  {'Reseñas':>7}  Producto")
    print("-" * 110)
    for i, p in enumerate(productos[:limite], 1):
        est = f"{p.estrellas:.1f}" if p.estrellas else " — "
        res = f"{p.resenas:,}" if p.resenas is not None else "—"
        titulo = p.titulo if len(p.titulo) <= 55 else p.titulo[:52] + "..."
        extra = " [cupón]" if p.cupon else ""
        print(
            f"{i:>3}  {p.descuento:>4.0f}%  {dinero(p.precio):>11}  {dinero(p.precio_lista):>11}  "
            f"{est:>3}  {res:>7}  {titulo}{extra}"
        )
        print(f"{'':>5}{p.url}")
    if len(productos) > limite:
        print(f"\n... y {len(productos) - limite} más en el CSV/HTML.")


def guardar_csv(productos: list[Producto], ruta: Path):
    campos = [
        "descuento", "puntaje", "precio", "precio_lista", "ahorro", "estrellas", "resenas",
        "titulo", "url", "keepa", "cupon", "prime", "patrocinado", "busquedas", "asin",
    ]
    with ruta.open("w", newline="", encoding="utf-8-sig") as f:  # utf-8-sig = acentos bien en Excel
        w = csv.DictWriter(f, fieldnames=campos)
        w.writeheader()
        for p in productos:
            d = asdict(p)
            d.update(descuento=p.descuento, puntaje=p.puntaje, ahorro=p.ahorro, keepa=p.keepa,
                     busquedas=", ".join(p.busquedas))
            w.writerow({k: d.get(k) for k in campos})


def guardar_html(productos: list[Producto], ruta: Path, titulo_busquedas: list[str]):
    e = html.escape
    tarjetas = []
    for i, p in enumerate(productos, 1):
        badges = []
        if p.prime:
            badges.append('<span class="b prime">Prime</span>')
        if p.cupon:
            badges.append(f'<span class="b cupon">{e(p.cupon)}</span>')
        if p.patrocinado:
            badges.append('<span class="b pat">Patrocinado</span>')
        estrellas = f"★ {p.estrellas:.1f}" if p.estrellas else "sin calificación"
        resenas = f"({p.resenas:,})" if p.resenas is not None else ""
        tarjetas.append(f"""
      <article>
        <div class="rank">{i}</div>
        <a class="img" href="{e(p.url)}" target="_blank" rel="noopener"><img src="{e(p.imagen)}" alt="" loading="lazy"></a>
        <div class="info">
          <a class="t" href="{e(p.url)}" target="_blank" rel="noopener">{e(p.titulo)}</a>
          <div class="precios">
            <span class="desc">-{p.descuento:.0f}%</span>
            <span class="precio">{dinero(p.precio)}</span>
            <s>{dinero(p.precio_lista)}</s>
            <span class="ahorro">ahorras {dinero(p.ahorro)}</span>
          </div>
          <div class="meta">{estrellas} {resenas} · puntaje {p.puntaje:.0f} · <em>{e(", ".join(p.busquedas))}</em></div>
          <div class="meta">{''.join(badges)} <a href="{e(p.keepa)}" target="_blank" rel="noopener">¿Precio real? Ver historial en Keepa</a></div>
        </div>
      </article>""")

    doc = f"""<!doctype html>
<html lang="es-MX">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Ofertas Amazon MX</title>
<style>
  :root {{ --bg:#f6f7f9; --card:#fff; --txt:#16181d; --sub:#5b6270; --acc:#c7511f; --ok:#0a7a3d; --line:#e3e6ea; }}
  @media (prefers-color-scheme: dark) {{
    :root {{ --bg:#111317; --card:#1b1e24; --txt:#eceef2; --sub:#9aa1ad; --acc:#ff8f4d; --ok:#4cd68a; --line:#2a2e36; }}
  }}
  * {{ box-sizing:border-box; }}
  body {{ margin:0; background:var(--bg); color:var(--txt); font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif; }}
  header {{ padding:20px 16px 8px; max-width:980px; margin:auto; }}
  h1 {{ margin:0 0 4px; font-size:22px; }}
  header p {{ margin:0; color:var(--sub); }}
  main {{ max-width:980px; margin:auto; padding:8px 16px 40px; display:grid; gap:10px; }}
  article {{ display:grid; grid-template-columns:28px 110px 1fr; gap:12px; background:var(--card); border:1px solid var(--line); border-radius:10px; padding:12px; align-items:start; }}
  .rank {{ color:var(--sub); font-weight:600; }}
  .img img {{ width:110px; height:110px; object-fit:contain; background:#fff; border-radius:6px; }}
  .t {{ color:var(--txt); text-decoration:none; font-weight:600; display:-webkit-box; -webkit-line-clamp:2; -webkit-box-orient:vertical; overflow:hidden; }}
  .t:hover {{ text-decoration:underline; }}
  .precios {{ margin:6px 0 2px; display:flex; flex-wrap:wrap; gap:8px; align-items:baseline; }}
  .desc {{ background:var(--acc); color:#fff; padding:1px 7px; border-radius:5px; font-weight:700; }}
  .precio {{ font-size:19px; font-weight:700; }}
  s {{ color:var(--sub); }}
  .ahorro {{ color:var(--ok); font-weight:600; }}
  .meta {{ color:var(--sub); font-size:13px; margin-top:3px; }}
  .meta a {{ color:var(--sub); }}
  .b {{ display:inline-block; font-size:12px; padding:0 6px; border-radius:4px; margin-right:4px; border:1px solid var(--line); }}
  .prime {{ color:#1a8cd8; border-color:#1a8cd8; }}
  .cupon {{ color:var(--ok); border-color:var(--ok); }}
  @media (max-width:560px) {{
    article {{ grid-template-columns:80px 1fr; }}
    .rank {{ display:none; }}
    .img img {{ width:80px; height:80px; }}
  }}
</style>
</head>
<body>
<header>
  <h1>Ofertas Amazon México · {len(productos)} encontradas</h1>
  <p>Búsquedas: {e(", ".join(titulo_busquedas))} · generado {datetime.now():%d/%m/%Y %H:%M}.
  Los precios pueden cambiar; confírmalos en Amazon antes de comprar.</p>
</header>
<main>{''.join(tarjetas) or '<p>Sin resultados con esos filtros.</p>'}
</main>
</body>
</html>
"""
    ruta.write_text(doc, encoding="utf-8")


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def leer_busquedas(a: argparse.Namespace) -> list[str]:
    busquedas = list(a.busquedas or [])
    if a.archivo:
        for linea in Path(a.archivo).read_text(encoding="utf-8").splitlines():
            linea = linea.strip()
            if linea and not linea.startswith("#"):
                busquedas.append(linea)
    # quitar duplicados respetando el orden
    vistos, out = set(), []
    for b in busquedas:
        if b.lower() not in vistos:
            vistos.add(b.lower())
            out.append(b)
    return out


def construir_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="Busca ofertas en Amazon México y las ordena por descuento real.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Ejemplos:\n"
            '  python buscar_ofertas.py "pantalla 55 pulgadas" --min-descuento 25 --min-estrellas 4\n'
            '  python buscar_ofertas.py -a busquedas.txt --precio-max 3000 --sin-patrocinados --abrir\n'
            '  python buscar_ofertas.py "ssd 1tb" --navegador   # si Amazon bloquea el modo normal\n'
        ),
    )
    ap.add_argument("busquedas", nargs="*", help="Palabras a buscar (cada una entre comillas)")
    ap.add_argument("-a", "--archivo", help="Archivo de texto con una búsqueda por línea")
    ap.add_argument("-p", "--paginas", type=int, default=2, help="Páginas de resultados por búsqueda (def. 2)")
    ap.add_argument("-d", "--min-descuento", type=float, default=20, help="Descuento mínimo en %% (def. 20)")
    ap.add_argument("--precio-min", type=float, help="Precio mínimo en pesos")
    ap.add_argument("--precio-max", type=float, help="Precio máximo en pesos")
    ap.add_argument("-e", "--min-estrellas", type=float, default=0, help="Calificación mínima, ej. 4.0")
    ap.add_argument("-r", "--min-resenas", type=int, default=0, help="Número mínimo de reseñas")
    ap.add_argument("--excluir", nargs="*", help="Palabras que NO quieres en el título (ej. funda reacondicionado)")
    ap.add_argument("--sin-patrocinados", action="store_true", help="Ocultar anuncios patrocinados")
    ap.add_argument("--solo-prime", action="store_true", help="Solo productos con Prime")
    ap.add_argument("-o", "--ordenar", choices=list(CLAVES_ORDEN), default="puntaje",
                    help="Cómo ordenar (def. puntaje = descuento ponderado por estrellas y reseñas)")
    ap.add_argument("--orden-amazon", choices=list(ORDEN_AMAZON), default="relevancia",
                    help="Orden en el que Amazon entrega los resultados")
    ap.add_argument("-n", "--mostrar", type=int, default=30, help="Cuántas mostrar en la terminal (def. 30)")
    ap.add_argument("--espera", type=float, default=3.0, help="Segundos entre páginas (def. 3; no lo bajes mucho)")
    ap.add_argument("--navegador", action="store_true", help="Usar un Chrome real con Playwright (evita bloqueos)")
    ap.add_argument("--oculto", action="store_true", help="Con --navegador, no mostrar la ventana")
    ap.add_argument("--salida", default="ofertas", help="Nombre base de los archivos de salida (def. ofertas)")
    ap.add_argument("--abrir", action="store_true", help="Abrir el reporte HTML al terminar")
    ap.add_argument("--guardar-html-crudo", action="store_true", help="Guardar el HTML de Amazon (para depurar)")
    return ap


def main(argv: list[str] | None = None) -> int:
    a = construir_parser().parse_args(argv)
    busquedas = leer_busquedas(a)
    if not busquedas:
        construir_parser().print_help()
        return 1

    desc = DescargadorNavegador(a.espera, a.oculto) if a.navegador else DescargadorRequests(a.espera)
    todos: dict[str, Producto] = {}
    bloqueos = 0

    try:
        for b in busquedas:
            print(f"\n🔎 {b}")
            for pag in range(1, a.paginas + 1):
                url = url_busqueda(b, pag, a.orden_amazon, a.precio_min, a.precio_max)
                contenido = desc.obtener(url)
                if contenido is None:
                    bloqueos += 1
                    print(f"   página {pag}: Amazon bloqueó la petición.")
                    break
                if a.guardar_html_crudo:
                    Path(f"crudo_{re.sub(r'[^a-z0-9]+', '_', b.lower())}_{pag}.html").write_text(contenido, encoding="utf-8")
                encontrados = parse_resultados(contenido, b)
                con_desc = sum(1 for p in encontrados if p.descuento > 0)
                print(f"   página {pag}: {len(encontrados)} productos, {con_desc} con descuento")
                for p in encontrados:
                    if p.asin in todos:
                        if b not in todos[p.asin].busquedas:
                            todos[p.asin].busquedas.append(b)
                    else:
                        p.busquedas = [b]
                        todos[p.asin] = p
                if not encontrados:
                    break
                time.sleep(a.espera + random.uniform(0.5, 2.0))
    except KeyboardInterrupt:
        print("\nInterrumpido; guardando lo que llevo...")
    finally:
        desc.cerrar()

    ofertas = sorted(filtrar(list(todos.values()), a), key=CLAVES_ORDEN[a.ordenar])
    imprimir_tabla(ofertas, a.mostrar)

    ruta_csv = Path(f"{a.salida}.csv")
    ruta_html = Path(f"{a.salida}.html")
    guardar_csv(ofertas, ruta_csv)
    guardar_html(ofertas, ruta_html, busquedas)
    print(f"\nRevisé {len(todos)} productos únicos → {len(ofertas)} ofertas pasan tus filtros.")
    print(f"Guardado: {ruta_csv.resolve()}\n          {ruta_html.resolve()}")

    if bloqueos and not a.navegador:
        print(
            "\n⚠️  Amazon bloqueó algunas peticiones. Opciones:\n"
            "   1) Espera unos minutos y sube --espera a 6\n"
            "   2) Usa el modo navegador:  pip install playwright && playwright install chromium\n"
            "      y luego agrega --navegador"
        )
    if a.abrir:
        webbrowser.open(ruta_html.resolve().as_uri())
    return 0


if __name__ == "__main__":
    sys.exit(main())
