// Deye Watcher — корпус для 3D-печати
// Плата: ESP32-S3 DevKitC-1 (модуль WROOM-1U) без впаянных гребёнок пинов,
// разъём RP-SMA для внешней антенны в торце, окно под USB-C в противоположном торце.
//
// Перед печатью ИЗМЕРЬТЕ СВОЮ ПЛАТУ штангенциркулем и поправьте pcb_l / pcb_w.
// Сборка: export base() и lid() отдельно (part = "base" / "lid" / "both").
//
// Печать: PETG или ABS/ASA (PLA размягчается у тёплого инвертора), слой 0.2, 3 периметра,
// заполнение 20%, без поддержек: основание — дном вниз, крышка — внешней стороной вниз.
// Крепёж крышки: 2 самореза 2.2–2.5 × 8 мм.

/* [Плата] */
pcb_l   = 63.5;   // длина платы по оси USB → модуль, мм (измерить!)
pcb_w   = 28.0;   // ширина платы, мм (измерить!)
pcb_t   = 1.6;    // толщина платы
clr     = 0.5;    // зазор вокруг платы
rail_h  = 3.0;    // высота опорных рельсов (место под детали снизу платы)
rail_w  = 1.6;    // ширина полки рельса

/* [Корпус] */
wall    = 2.0;
floor_t = 2.0;
lid_t   = 2.0;
inner_h = 16.0;   // от пола до крышки
ant_zone= 15.0;   // свободное место за платой под разъём RP-SMA и кабель U.FL

/* [Отверстия] */
usb_w   = 24.0;   // окно под USB-C (оба разъёма платы), ширина
usb_h   = 9.0;    // высота окна
usb_z   = 6.4;    // центр окна над внутренним полом (рельс + плата + половина разъёма)
sma_d   = 6.6;    // отверстие под RP-SMA (резьба 1/4"-36 ≈ 6.35 мм)
sma_z   = 9.0;    // центр отверстия над внутренним полом
nut_af  = 8.3;    // шестигранный карман под внутреннюю гайку RP-SMA (8 мм под ключ + зазор)
nut_t   = 2.2;    // глубина кармана
boss_t  = 3.0;    // утолщение торцевой стенки изнутри
vent_n  = 4;      // вентиляционные прорези на каждой длинной стенке
ear     = true;   // ушки для крепления к стене (шуруп ⌀4)

part    = "both"; // "base", "lid", "both"
$fn     = 48;

in_l = pcb_l + 2*clr + ant_zone;
in_w = pcb_w + 2*clr;
out_l = in_l + 2*wall;
out_w = in_w + 2*wall;
out_h = floor_t + inner_h;            // высота основания без крышки

post_d = 6.0; post_hole = 2.0;        // стойки под саморезы крышки (в зоне антенны)
post_x = out_l - wall - post_d/2 + 0.6; // у торца с антенной, слегка утоплены в стенку
post_y = [wall + post_d/2 - 0.6, out_w - wall - post_d/2 + 0.6];   // слегка «утоплены» в стенки

module rounded_box(l, w, h, r=2) {
  hull() for (x=[r, l-r], y=[r, w-r]) translate([x, y, 0]) cylinder(r=r, h=h);
}

module base() {
  difference() {
    union() {
      difference() {
        rounded_box(out_l, out_w, out_h);
        translate([wall, wall, floor_t]) cube([in_l, in_w, inner_h + 1]);
      }
      // опорные рельсы вдоль длинных стенок (под краями платы)
      for (y=[wall - 0.5, out_w - wall - rail_w])
        translate([wall - 0.5, y, floor_t - 0.5]) cube([pcb_l + 2*clr + 0.5, rail_w + 0.5, rail_h + 0.5]);
      // упоры за краем платы (держат плату у окна USB)
      for (y=[wall - 0.5, out_w - wall - 3])
        translate([wall + pcb_l + 2*clr, y, floor_t - 0.5]) cube([1.6, 3.5, rail_h + pcb_t + 2.0]);
      // стойки под саморезы крышки
      for (y=post_y) translate([post_x, y, floor_t - 0.5]) cylinder(d=post_d, h=inner_h + 0.5);
      // утолщение торца под гнездо антенны (ниже бортика крышки)
      translate([out_l - wall - boss_t, out_w/2 - 7, floor_t - 0.5]) cube([boss_t + 0.5, 14, inner_h - 1.6 - 0.4 + 0.5]);
      // ушки для крепления
      if (ear) for (x=[-10, out_l - 0.5]) translate([x, out_w/2 - 6, 0])
        difference() {
          cube([10.5, 12, 3]);
          translate([x < 0 ? 5 : 5.5, 6, -1]) cylinder(d=4.5, h=5);
        }
    }
    // отверстия в стойках
    for (y=post_y) translate([post_x, y, floor_t + 2]) cylinder(d=post_hole, h=inner_h);
    // окно USB-C (торец x=0)
    translate([-1, out_w/2 - usb_w/2, floor_t + usb_z - usb_h/2]) cube([wall + 2, usb_w, usb_h]);
    // отверстие RP-SMA (торец x=out_l)
    translate([out_l - wall - boss_t - 1, out_w/2, floor_t + sma_z]) rotate([0, 90, 0]) cylinder(d=sma_d, h=wall + boss_t + 2);
    // шестигранный карман под внутреннюю гайку (вершиной вверх — печатается без поддержек)
    translate([out_l - wall - boss_t - 0.01, out_w/2, floor_t + sma_z]) rotate([0, 90, 0]) rotate([0, 0, 30])
      cylinder(d=nut_af/cos(30), h=nut_t + 0.01, $fn=6);
    // вентиляция на длинных стенках
    for (i=[0:vent_n-1], y=[-1, out_w - wall - 1])
      translate([wall + 8 + i*(pcb_l - 10)/max(vent_n-1,1) - 0.75, y, floor_t + rail_h + pcb_t + 3])
        cube([1.5, wall + 2, 7]);
  }
}

module lid() {
  lip = 1.6;   // бортик, входящий внутрь стенок
  difference() {
    union() {
      rounded_box(out_l, out_w, lid_t);
      // бортик по периметру внутрь корпуса (зазор 0.25 мм)
      translate([wall + 0.25, wall + 0.25, lid_t]) difference() {
        cube([in_l - 0.5, in_w - 0.5, lip]);
        translate([1.2, 1.2, -1]) cube([in_l - 0.5 - 2.4, in_w - 0.5 - 2.4, lip + 2]);
      }
      // прижимные рёбра над краями платы (начинаются за кнопками BOOT/RST у торца USB)
      for (y=[wall + 0.6, out_w - wall - 0.6 - 1.2])
        translate([wall + 14, y, lid_t]) cube([pcb_l - 16, 1.2, inner_h - rail_h - pcb_t - 0.2]);
    }
    // выемки в бортике под стойки
    for (y=post_y) translate([post_x, y, lid_t - 0.01]) cylinder(d=post_d + 0.6, h=lip + 1);
    // отверстия под саморезы с потаем
    for (y=post_y) translate([post_x, y, -1]) {
      cylinder(d=2.8, h=lid_t + 2);
      cylinder(d1=5.2, d2=2.8, h=1.8);
    }
    // вырез в бортике у окна USB, чтобы штекер входил полностью
    translate([wall - 1, out_w/2 - usb_w/2, lid_t - 0.01]) cube([3, usb_w, lip + 1]);
  }
}

if (part == "base") base();
else if (part == "lid") lid();
else if (part == "both") {
  base();
  translate([0, out_w + 12, 0]) lid();
}
