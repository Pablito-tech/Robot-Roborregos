import argparse
import sys
from pathlib import Path

AQUI = Path(__file__).parent
sys.path.insert(0, str(AQUI.parent))

import buscar_ofertas as bo  # noqa: E402

HTML = (AQUI / "fixtures" / "resultados.html").read_text(encoding="utf-8")


def args(**kw):
    a = bo.construir_parser().parse_args([])
    for k, v in kw.items():
        setattr(a, k, v)
    return a


def test_parse_precio():
    assert bo.parse_precio("$1,299.00") == 1299.0
    assert bo.parse_precio("MXN 499") == 499.0
    assert bo.parse_precio("") is None
    assert bo.parse_precio(None) is None


def test_parse_estrellas_y_resenas():
    assert bo.parse_estrellas("4.6 de 5 estrellas") == 4.6
    assert bo.parse_estrellas("2,8 de 5 estrellas") == 2.8
    assert bo.parse_resenas("12,345 calificaciones") == 12345
    assert bo.parse_resenas("(2.5 mil)") == 2500
    assert bo.parse_resenas("12K") == 12000


def test_parse_resultados():
    ps = {p.asin: p for p in bo.parse_resultados(HTML, "audifonos")}
    assert set(ps) == {"B0AAAA0001", "B0AAAA0002", "B0AAAA0003", "B0AAAA0004"}

    a = ps["B0AAAA0001"]
    assert a.precio == 1299.0 and a.precio_lista == 2599.0
    assert a.descuento == 50.0
    assert a.estrellas == 4.6 and a.resenas == 12345
    assert a.prime and not a.patrocinado
    assert "cupón" in a.cupon
    assert a.url == "https://www.amazon.com.mx/dp/B0AAAA0001"

    c = ps["B0AAAA0003"]
    assert c.patrocinado
    assert c.titulo == "Audífonos Marca X Pro"
    assert c.descuento == 0 and c.resenas == 2500

    assert ps["B0AAAA0004"].precio is None


def test_puntaje_premia_productos_confiables():
    ps = {p.asin: p for p in bo.parse_resultados(HTML)}
    bueno, dudoso = ps["B0AAAA0001"], ps["B0AAAA0002"]
    assert dudoso.descuento > bueno.descuento  # 80% vs 50%
    assert bueno.puntaje > dudoso.puntaje      # pero el bueno es mejor oferta


def test_filtros():
    ps = bo.parse_resultados(HTML)
    ids = lambda xs: {p.asin for p in xs}
    assert ids(bo.filtrar(ps, args(min_descuento=20))) == {"B0AAAA0001", "B0AAAA0002"}
    assert ids(bo.filtrar(ps, args(min_descuento=20, min_estrellas=4))) == {"B0AAAA0001"}
    assert ids(bo.filtrar(ps, args(min_descuento=0, sin_patrocinados=True))) == {"B0AAAA0001", "B0AAAA0002"}
    assert ids(bo.filtrar(ps, args(min_descuento=0, precio_max=500))) == {"B0AAAA0002"}
    assert ids(bo.filtrar(ps, args(min_descuento=0, excluir=["genéricos"]))) == {"B0AAAA0001", "B0AAAA0003"}


def test_captcha():
    assert bo.es_captcha('<form action="/errors/validateCaptcha">')
    assert not bo.es_captcha(HTML)


def test_salidas(tmp_path):
    ps = bo.filtrar(bo.parse_resultados(HTML, "audifonos"), args(min_descuento=0))
    for p in ps:
        p.busquedas = ["audifonos"]
    bo.guardar_csv(ps, tmp_path / "o.csv")
    bo.guardar_html(ps, tmp_path / "o.html", ["audifonos"])
    assert "B0AAAA0001" in (tmp_path / "o.csv").read_text(encoding="utf-8-sig")
    h = (tmp_path / "o.html").read_text(encoding="utf-8")
    assert "keepa.com/#!product/11-B0AAAA0001" in h and "-50%" in h
