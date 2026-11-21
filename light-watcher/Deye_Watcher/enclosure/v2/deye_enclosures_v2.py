"""Deye Watcher — корпуса v2 (CadQuery): скруглённые углы, фаски, теневой шов, маркировка.

Посадочные размеры (плата, окно USB-C, гнездо RP-SMA с карманом под гайку, бобышки,
защёлка DIN) взяты из v1 (deye_watcher_din.scad / deye_watcher_case.scad) без изменений.

Запуск:  pip install cadquery trimesh  &&  python3 deye_enclosures_v2.py [папка_вывода]
Результат: STL в ориентации для печати + STEP для правки в CAD.
"""
import math
import os
import sys

import cadquery as cq

FONT_BOLD = "/usr/share/fonts/opentype/inter/InterDisplay-Bold.otf"
FONT_SEMI = "/usr/share/fonts/opentype/inter/InterDisplay-SemiBold.otf"
if not os.path.exists(FONT_BOLD):          # запасной вариант — системный шрифт
    FONT_BOLD = FONT_SEMI = None


def box(x0, y0, z0, sx, sy, sz):
    return cq.Workplane("XY").box(sx, sy, sz, centered=False).translate((x0, y0, z0))


def cyl(x, y, z0, d, h):
    return cq.Workplane("XY").circle(d / 2).extrude(h).translate((x, y, z0))


def cone(x, y, z0, d1, d2, h):
    """Усечённый конус d1 (низ) -> d2 (верх)."""
    return cq.Workplane("XY").add(cq.Solid.makeCone(max(d1, 1e-3) / 2, d2 / 2, h)).translate((x, y, z0))


def rbox(x0, y0, z0, sx, sy, sz, r, axis="|Z"):
    b = box(x0, y0, z0, sx, sy, sz)
    return b.edges(axis).fillet(r) if r > 0 else b


def hex_pts(af):
    """Шестигранник под ключ af, вершины по локальной оси Y."""
    r = af / math.cos(math.radians(30)) / 2
    return [(r * math.sin(math.radians(a)), r * math.cos(math.radians(a))) for a in range(0, 360, 60)]


def text3d(s, size, depth, font, kind="bold"):
    kw = dict(fontPath=font) if font else dict(kind=kind)
    return cq.Workplane("XY").text(s, size, depth, halign="center", valign="center", **kw)


# =====================================================================================
#  DIN-рейка TS35, 2 модуля
# =====================================================================================
class Din:
    pcb_l, pcb_w, pcb_t, under = 63.5, 28.0, 1.6, 4.0
    mod_w, dev_h, wall, back_t = 36.0, 90.0, 2.0, 2.5
    body_d, neck_h, front_d, cap_t = 40.0, 45.0, 54.0, 2.0
    usb_w, usb_h = 24.0, 9.0
    sma_d, sma_y, nut_af, nut_t, boss_t = 6.6, 52.0, 8.3, 2.2, 3.0
    boss_d, boss_hole = 6.0, 2.0
    R = 3.0            # радиус продольных рёбер корпуса
    R_FRONT = 2.0      # радиус кромок лицевой панели
    SEAM = 0.4         # фаска теневого шва между основанием и крышкой

    def __init__(self):
        s = self
        s.inner_w = s.mod_w - 2 * s.wall
        s.zb = s.back_t + s.under
        s.pcb_x0 = (s.mod_w - s.pcb_w) / 2
        s.bx = [s.wall + s.boss_d / 2 - 0.6, s.mod_w - s.wall - s.boss_d / 2 + 0.6]
        s.by_top = s.dev_h - s.wall - s.boss_d / 2 + 0.6
        s.by_bot = s.wall + s.boss_d / 2 - 0.6
        s.hang_z = s.zb + s.pcb_t + 5
        s.clip_y = [s.dev_h / 2 - 6, s.dev_h / 2 + 10]
        s.nd = s.front_d - s.body_d

    def body(self):
        s = self
        outer = rbox(0, 0, 0, s.mod_w, s.dev_h, s.body_d, s.R)
        outer = outer.faces("<Z").chamfer(0.8)          # на столе: фаска вместо скругления
        outer = outer.faces(">Z").chamfer(s.SEAM)       # половина теневого шва
        cavity = rbox(s.wall, s.wall, s.back_t, s.inner_w, s.dev_h - 2 * s.wall, s.body_d, 1.0)
        add = cq.Workplane("XY")
        for mir in (False, True):                        # лоток платы: полка + бортик
            a = box(s.wall - 0.5, s.wall - 0.5, s.back_t - 0.5, s.pcb_x0 - s.wall + 2.0, s.pcb_l + 0.8, s.under + 0.5)
            b = box(s.wall - 0.5, s.wall - 0.5, s.back_t - 0.5, s.pcb_x0 - s.wall + 0.2, s.pcb_l + 0.8, s.under + s.pcb_t + 1.5)
            ab = a.union(b)
            if mir:
                ab = ab.mirror("YZ", (s.mod_w / 2, 0, 0))
            add = add.union(ab)
        add = add.union(box(s.wall - 0.5, s.wall + s.pcb_l + 0.3, s.back_t - 0.5, s.inner_w + 1, 1.6, s.under + s.pcb_t + 2))
        for x in s.bx:
            add = add.union(cyl(x, s.by_top, s.back_t - 0.5, s.boss_d, s.body_d - s.back_t + 0.5))
            add = add.union(cyl(x, s.by_bot, s.hang_z, s.boss_d, s.body_d - s.hang_z))
            add = add.union(cone(x, s.by_bot, s.hang_z - 3, 0.6, s.boss_d, 3))
        for y in s.clip_y:
            add = add.union(cyl(s.mod_w / 2, y, s.back_t - 0.5, 7, 2.5))
        body = outer.cut(cavity).union(add.intersect(outer))
        # отверстия
        for x in s.bx:
            for y in (s.by_top, s.by_bot):
                body = body.cut(cyl(x, y, s.body_d - 9, s.boss_hole, 10))
        for y in s.clip_y:
            body = body.cut(cyl(s.mod_w / 2, y, -1, 2.8, 10))
            body = body.cut(cone(s.mod_w / 2, y, s.back_t + 0.4, 2.8, 5.4, 1.61))
        # окно USB-C со скруглёнными углами
        uz = s.zb + s.pcb_t + 1.6
        body = body.cut(rbox(s.mod_w / 2 - s.usb_w / 2, -1, uz - s.usb_h / 2, s.usb_w, s.wall + 2, s.usb_h, 1.5, "|Y"))
        # вентиляция: 5 овальных прорезей в верхней стенке
        for i in range(5):
            slot = cq.Workplane("XZ").slot2D(20, 2.0, 90).extrude(-(s.wall + 2))
            body = body.cut(slot.translate((s.mod_w / 2 + (i - 2) * 4.5, s.dev_h - s.wall - 1, 24)))
        return body

    def cap(self):
        s = self
        y0 = s.dev_h / 2 - s.neck_h / 2
        plate = rbox(0, 0, 0, s.mod_w, s.dev_h, s.cap_t, s.R)
        plate = plate.faces("<Z").chamfer(s.SEAM).faces(">Z").chamfer(0.5)
        nose = rbox(0, y0, 0, s.mod_w, s.neck_h, s.nd, s.R).faces(">Z").edges().fillet(s.R_FRONT)
        cap = plate.union(nose)
        cap = cap.union(box(s.wall - 0.5, s.sma_y - 7, s.nd - s.cap_t - s.boss_t, s.inner_w + 1, 14, s.boss_t + 0.5).intersect(nose))
        cap = cap.cut(rbox(s.wall, y0 + s.wall, -1, s.inner_w, s.neck_h - 2 * s.wall, s.nd - s.cap_t + 1, 1.0))
        # восстановить ребро под гнездо (полость его срезала)
        cap = cap.union(box(s.wall - 0.5, s.sma_y - 7, s.nd - s.cap_t - s.boss_t, s.inner_w + 1, 14, s.boss_t + 0.5).intersect(nose))
        cx = s.mod_w / 2
        cap = cap.cut(cyl(cx, s.sma_y, s.nd - s.cap_t - s.boss_t - 1, s.sma_d, s.cap_t + s.boss_t + 2))
        pocket = cq.Workplane("XY").polyline(hex_pts(s.nut_af)).close().extrude(s.nut_t + 0.01)
        cap = cap.cut(pocket.translate((cx, s.sma_y, s.nd - s.cap_t - s.boss_t - 0.01)))
        for x in s.bx:
            for y in (s.by_top, s.by_bot):
                cap = cap.cut(cyl(x, y, -1, 2.8, s.cap_t + 2))
                cap = cap.cut(cone(x, y, s.cap_t - 1.6, 2.8, 5.4, 1.61))
        # ---- лицевая панель ----
        zf = s.nd
        cap = cap.cut(cyl(cx, s.sma_y, zf - 0.6, 12.5, 1))                      # посадка шайбы/гайки
        ring = cq.Workplane("XY").circle(8.6).circle(8.0).extrude(1)
        cap = cap.cut(ring.translate((cx, s.sma_y, zf - 0.4)))                  # кольцо-акцент
        cap = cap.cut(text3d("DEYE", 6.2, 1, FONT_BOLD).translate((cx, y0 + 13.6, zf - 0.6)))
        cap = cap.cut(text3d("WATCHER", 3.1, 1, FONT_SEMI).translate((cx, y0 + 7.6, zf - 0.6)))
        cap = cap.cut(box(cx - 9, y0 + 19.2, zf - 0.4, 18, 0.6, 1))             # линия-разделитель
        return cap

    def pcb(self):
        s = self
        p = box(s.pcb_x0, s.wall, s.zb, s.pcb_w, s.pcb_l, s.pcb_t)
        p = p.union(box(s.pcb_x0 + 3, s.wall, s.zb + s.pcb_t, s.pcb_w - 6, 8, 3.3))
        return p.union(box(s.pcb_x0 + (s.pcb_w - 18) / 2, s.wall + s.pcb_l - 26, s.zb + s.pcb_t, 18, 25.5, 5.7))


# =====================================================================================
#  Настенный корпус
# =====================================================================================
class Wall:
    pcb_w, pcb_t, clr, rail_h, rail_w = 28.0, 1.6, 0.5, 3.0, 1.6
    wall, floor_t, lid_t, inner_h, ant_zone = 2.0, 2.0, 2.0, 16.0, 15.0
    usb_w, usb_h, usb_z = 24.0, 9.0, 6.4
    sma_d, sma_z, nut_af, nut_t, boss_t = 6.6, 9.0, 8.3, 2.2, 3.0
    post_d, post_hole = 6.0, 2.0
    R = 3.5            # радиус вертикальных углов
    R_IN = 1.5
    EDGE = 1.0         # фаска по низу основания и верху крышки
    SEAM = 0.4

    def __init__(self, pcb_l=63.5):
        s = self
        s.pcb_l = pcb_l
        s.in_l = pcb_l + 2 * s.clr + s.ant_zone
        s.in_w = s.pcb_w + 2 * s.clr
        s.out_l = s.in_l + 2 * s.wall
        s.out_w = s.in_w + 2 * s.wall
        s.out_h = s.floor_t + s.inner_h
        s.post_x = s.out_l - s.wall - s.post_d / 2 + 0.6
        s.post_y = [s.wall + s.post_d / 2 - 0.6, s.out_w - s.wall - s.post_d / 2 + 0.6]

    def ear(self, end):
        """Ушко с полукруглым концом и плавным переходом к корпусу. end = 0 (USB) или 1 (антенна)."""
        s = self
        cy, r, t, ext = s.out_w / 2, 6.0, 3.0, 5.0
        e = box(-ext, cy - r, 0, ext + s.R + 1, 2 * r, t).union(cyl(-ext, cy, 0, 2 * r, t))
        for sgn in (-1, 1):                              # галтели R2 у стенки
            patch = box(-2, cy + sgn * r - (2 if sgn < 0 else 0), 0, 2, 2, t)
            patch = patch.cut(cyl(-2, cy + sgn * (r + 2), -1, 4, t + 2))
            e = e.union(patch)
        e = e.cut(cyl(-ext, cy, -1, 4.5, t + 2)).cut(cone(-ext, cy, t - 1.2, 4.5, 6.9, 1.21))
        if end:
            e = e.mirror("YZ", (s.out_l / 2, 0, 0))
        return e

    def base(self, ears=True):
        s = self
        outer = rbox(0, 0, 0, s.out_l, s.out_w, s.out_h, s.R)
        outer = outer.faces("<Z").chamfer(s.EDGE).faces(">Z").chamfer(s.SEAM)
        cavity = rbox(s.wall, s.wall, s.floor_t, s.in_l, s.in_w, s.inner_h + 1, s.R_IN)
        add = cq.Workplane("XY")
        for y in (s.wall - 0.5, s.out_w - s.wall - s.rail_w):
            add = add.union(box(s.wall - 0.5, y, s.floor_t - 0.5, s.pcb_l + 2 * s.clr + 0.5, s.rail_w + 0.5, s.rail_h + 0.5))
        for y in (s.wall - 0.5, s.out_w - s.wall - 3):
            add = add.union(box(s.wall + s.pcb_l + 2 * s.clr, y, s.floor_t - 0.5, 1.6, 3.5, s.rail_h + s.pcb_t + 2.0))
        for y in s.post_y:
            add = add.union(cyl(s.post_x, y, s.floor_t - 0.5, s.post_d, s.inner_h + 0.5))
        add = add.union(box(s.out_l - s.wall - s.boss_t, s.out_w / 2 - 7, s.floor_t - 0.5, s.boss_t + 0.5, 14, s.inner_h - 1.6 - 0.4 + 0.5))
        base = outer.cut(cavity).union(add.intersect(outer))
        if ears:
            base = base.union(self.ear(0)).union(self.ear(1))
            base = base.cut(cavity.intersect(box(s.wall, s.wall, s.floor_t, s.in_l, s.in_w, 1.2)))  # ушки не лезут внутрь
        for y in s.post_y:
            base = base.cut(cyl(s.post_x, y, s.floor_t + 2, s.post_hole, s.inner_h))
        # окно USB-C
        base = base.cut(rbox(-1, s.out_w / 2 - s.usb_w / 2, s.floor_t + s.usb_z - s.usb_h / 2, s.wall + 2, s.usb_w, s.usb_h, 1.5, "|X"))
        # RP-SMA + карман под гайку (вершиной вверх — печать без поддержек) + посадка наружной гайки
        zc = s.floor_t + s.sma_z
        yz = cq.Workplane("YZ")
        base = base.cut(yz.circle(s.sma_d / 2).extrude(s.wall + s.boss_t + 2).translate((s.out_l - s.wall - s.boss_t - 1, s.out_w / 2, zc)))
        base = base.cut(yz.polyline(hex_pts(s.nut_af)).close().extrude(s.nut_t + 0.01).translate((s.out_l - s.wall - s.boss_t - 0.01, s.out_w / 2, zc)))
        base = base.cut(yz.circle(6.25).extrude(1).translate((s.out_l - 0.6, s.out_w / 2, zc)))
        # вентиляция: группа овальных прорезей на длинных стенках
        z0 = s.floor_t + s.rail_h + s.pcb_t + 3
        n, pitch, xc = 7, 3.6, s.wall + s.clr + s.pcb_l / 2 + 6
        for i in range(n):
            for y in (-1, s.out_w - s.wall - 1):
                slot = cq.Workplane("XZ").slot2D(6.8, 1.6, 90).extrude(-(s.wall + 2))
                base = base.cut(slot.translate((xc + (i - (n - 1) / 2) * pitch, y, z0 + 3.4)))
        return base

    def lid(self):
        """Крышка в положении сборки: внешняя плоскость сверху (z = out_h + lid_t)."""
        s = self
        zt = s.out_h
        lip, g = 1.6, 0.25
        lid = rbox(0, 0, zt, s.out_l, s.out_w, s.lid_t, s.R)
        lid = lid.faces(">Z").chamfer(s.EDGE).faces("<Z").chamfer(s.SEAM)
        frame = rbox(s.wall + g, s.wall + g, zt - lip, s.in_l - 2 * g, s.in_w - 2 * g, lip, s.R_IN - g)
        frame = frame.cut(box(s.wall + g + 1.2, s.wall + g + 1.2, zt - lip - 1, s.in_l - 2 * g - 2.4, s.in_w - 2 * g - 2.4, lip + 2))
        lid = lid.union(frame)
        rib_h = s.inner_h - s.rail_h - s.pcb_t - 0.2
        for y in (s.wall + 0.6, s.out_w - s.wall - 0.6 - 1.2):
            lid = lid.union(box(s.wall + 14, y, zt - rib_h, s.pcb_l - 16, 1.2, rib_h))
        for y in s.post_y:
            lid = lid.cut(cyl(s.post_x, y, zt - lip - 1, s.post_d + 0.6, lip + 1.01))
            lid = lid.cut(cyl(s.post_x, y, zt - 1, 2.8, s.lid_t + 2))
            lid = lid.cut(cone(s.post_x, y, zt + s.lid_t - 1.8, 2.8, 5.2, 1.81))
        lid = lid.cut(box(s.wall - 1, s.out_w / 2 - s.usb_w / 2, zt - lip - 1, 3, s.usb_w, lip + 1.01))
        # ---- маркировка на крышке ----
        ztop = zt + s.lid_t
        x_txt = s.wall + s.clr + s.pcb_l * 0.36
        lid = lid.cut(text3d("DEYE", 6.6, 1, FONT_BOLD).translate((x_txt - 9.5, s.out_w / 2, ztop - 0.4)))
        lid = lid.cut(text3d("WATCHER", 3.3, 1, FONT_SEMI).translate((x_txt + 12.5, s.out_w / 2 - 0.1, ztop - 0.4)))
        lid = lid.cut(box(x_txt + 1.2, s.out_w / 2 - 3.6, ztop - 0.4, 0.6, 7.2, 1))
        return lid

    def pcb(self):
        s = self
        z = s.floor_t + s.rail_h
        p = box(s.wall + s.clr, s.wall + s.clr, z, s.pcb_l, s.pcb_w, s.pcb_t)
        p = p.union(box(s.wall + s.clr, s.out_w / 2 - 11, z + s.pcb_t, 8, 22, 3.3))
        return p.union(box(s.wall + s.clr + s.pcb_l - 26, s.out_w / 2 - 9, z + s.pcb_t, 25.5, 18, 5.7))


# =====================================================================================
def export(obj, path, tol=0.02, ang=0.25):
    cq.exporters.export(obj, path, tolerance=tol, angularTolerance=ang)


def main(out):
    os.makedirs(out, exist_ok=True)
    d = Din()
    body, cap = d.body(), d.cap()
    export(body, f"{out}/deye_din_v2_body_pcb63.5.stl")           # задней стенкой на стол
    export(cap, f"{out}/deye_din_v2_cap_pcb63.5.stl")             # плечами на стол, выступ вверх
    export(body, f"{out}/deye_din_v2_body_pcb63.5.step")
    export(cap, f"{out}/deye_din_v2_cap_pcb63.5.step")
    for pl in (63.5, 69.0):
        w = Wall(pl)
        tag = f"pcb{pl:g}"
        base, lid = w.base(), w.lid()
        lid_print = lid.rotate((0, 0, 0), (1, 0, 0), 180).translate((0, w.out_w, w.out_h + w.lid_t))  # внешней стороной вниз
        export(base, f"{out}/deye_case_v2_base_{tag}.stl")
        export(lid_print, f"{out}/deye_case_v2_lid_{tag}.stl")
        export(base, f"{out}/deye_case_v2_base_{tag}.step")
        export(lid, f"{out}/deye_case_v2_lid_{tag}.step")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__)))
