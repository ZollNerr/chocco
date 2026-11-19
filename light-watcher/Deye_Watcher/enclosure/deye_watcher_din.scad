// Deye Watcher — корпус на DIN-рейку TS35 (2 модуля = 36 мм)
// Плата ESP32-S3 DevKitC-1 (WROOM-1U) лежит в лотке параллельно задней стенке, USB-C смотрит вниз,
// разъём RP-SMA антенны — на лицевой панели выступа 45 мм (выходит через вырез крышки щита).
//
// Детали (part = ...):
//   "body"     — основание (печать задней стенкой на стол)
//   "cap"      — крышка с выступом (печать плечами на стол, выступ вверх)
//   "clip"     — защёлка DIN (печать плашмя: пружина гнётся вдоль слоёв)
//   "assembly" — сборка с рейкой, только для просмотра/проверки
// Материал PETG/ASA, слой 0.2, 3 периметра, 20 %, поддержки не нужны.
// Крепёж: 6 саморезов 2.2×8 мм (4 — крышка, 2 — защёлка изнутри), полоска двустороннего скотча под плату.

/* [Плата] */
pcb_l  = 63.5;   // длина платы — ИЗМЕРЬТЕ свою
pcb_w  = 28.0;   // ширина платы
pcb_t  = 1.6;
under  = 4.0;    // зазор под платой (детали снизу, головки саморезов защёлки)

/* [Корпус] */
mod_w   = 36;    // 2 модуля DIN
dev_h   = 90;    // высота модульного прибора
wall    = 2;
back_t  = 2.5;
body_d  = 40;    // глубина основания: плечи на 44 мм от рейки (с защёлкой 4 мм)
neck_h  = 45;    // высота выступа под вырез крышки щита
front_d = 54;    // лицевая панель: 58 мм от рейки
cap_t   = 2;

/* [Отверстия] */
usb_w = 24; usb_h = 9;
sma_d = 6.6; sma_y = 52;
// гнездо RP-SMA (резьба 1/4"-36): внутренняя гайка 8 мм под ключ сидит в шестигранном кармане,
// поэтому антенну можно вкручивать/выкручивать сколько угодно — разъём не проворачивается
nut_af = 8.3;    // размер под ключ кармана (гайка 8 мм + зазор)
nut_t  = 2.2;    // глубина кармана
boss_t = 3.0;    // утолщение лицевой панели изнутри под карманом

/* [Защёлка] */
clip_w   = 12;
rail_top = dev_h/2 + 17.5;
rail_bot = dev_h/2 - 17.5;

part = "assembly";
$fn = 40;

inner_w  = mod_w - 2*wall;
zb       = back_t + under;            // низ платы
pcb_x0   = (mod_w - pcb_w)/2;         // плата по центру
boss_d   = 6; boss_hole = 2.0;
bx = [wall + boss_d/2 - 0.6, mod_w - wall - boss_d/2 + 0.6];
by_top = dev_h - wall - boss_d/2 + 0.6;
by_bot = wall + boss_d/2 - 0.6;
hang_z = zb + pcb_t + 5;               // нижние бобышки начинаются выше деталей платы
clip_screw_y = [dev_h/2 - 6, dev_h/2 + 10];

// ---------------- основание ----------------
module body() {
  difference() {
    union() {
      difference() {
        cube([mod_w, dev_h, body_d]);
        translate([wall, wall, back_t]) cube([inner_w, dev_h - 2*wall, body_d]);
      }
      // лоток: опорные рёбра под краями платы + боковые бортики
      for (s=[0,1]) translate([s*mod_w, 0, 0]) mirror([s, 0, 0]) {
        translate([wall - 0.5, wall - 0.5, back_t - 0.5]) cube([pcb_x0 - wall + 1.5 + 0.5, pcb_l + 0.8, under + 0.5]);
        translate([wall - 0.5, wall - 0.5, back_t - 0.5]) cube([pcb_x0 - wall - 0.3 + 0.5, pcb_l + 0.8, under + pcb_t + 1.5]);
      }
      // упор над верхним краем платы
      translate([wall - 0.5, wall + pcb_l + 0.3, back_t - 0.5]) cube([inner_w + 1, 1.6, under + pcb_t + 2]);
      // бобышки крышки: верхние — на всю высоту
      for (x=bx) translate([x, by_top, back_t - 0.5]) cylinder(d=boss_d, h=body_d - back_t + 0.5);
      // нижние — «подвесные» над платой, с конусом 45° снизу (печать без поддержек)
      for (x=bx) translate([x, by_bot, hang_z]) {
        cylinder(d=boss_d, h=body_d - hang_z);
        translate([0, 0, -3]) cylinder(d1=0.6, d2=boss_d, h=3);
      }
      // площадки под саморезы защёлки
      for (y=clip_screw_y) translate([mod_w/2, y, back_t - 0.5]) cylinder(d=7, h=2.5);
    }
    for (x=bx, y=[by_top, by_bot]) translate([x, y, body_d - 9]) cylinder(d=boss_hole, h=10);
    // саморезы защёлки: изнутри наружу, потай у площадки
    for (y=clip_screw_y) translate([mod_w/2, y, -1]) {
      cylinder(d=2.8, h=10);
      translate([0, 0, back_t + 2 - 1.6 + 1]) cylinder(d1=2.8, d2=5.4, h=1.61);
    }
    // окно USB-C в нижней стенке
    translate([mod_w/2 - usb_w/2, -1, zb + pcb_t + 1.6 - usb_h/2]) cube([usb_w, wall + 2, usb_h]);
    // вентиляция в верхней стенке
    for (i=[0:3]) translate([10 + i*5.5, dev_h - wall - 1, 18]) cube([2, wall + 2, 16]);
  }
}

// ---------------- крышка с выступом ----------------
module cap() {
  nd = front_d - body_d;               // высота выступа над основанием
  difference() {
    union() {
      cube([mod_w, dev_h, cap_t]);                                       // плечи
      translate([0, dev_h/2 - neck_h/2, 0]) cube([mod_w, neck_h, nd]);   // выступ
      // ребро-утолщение под гнездом антенны: поперёк выступа от стенки до стенки (печатается мостом)
      translate([wall - 0.5, sma_y - 7, nd - cap_t - boss_t]) cube([inner_w + 1, 14, boss_t + 0.5]);
    }
    // полость выступа, открытая внутрь корпуса
    translate([wall, dev_h/2 - neck_h/2 + wall, -1]) cube([inner_w, neck_h - 2*wall, nd - cap_t + 1]);
    // RP-SMA на лицевой панели
    translate([mod_w/2, sma_y, nd - cap_t - boss_t - 1]) cylinder(d=sma_d, h=cap_t + boss_t + 2);
    // шестигранный карман под внутреннюю гайку
    translate([mod_w/2, sma_y, nd - cap_t - boss_t - 0.01]) cylinder(d=nut_af/cos(30), h=nut_t + 0.01, $fn=6);
    // 4 самореза с потаем
    for (x=bx, y=[by_top, by_bot]) translate([x, y, -1]) {
      cylinder(d=2.8, h=cap_t + 2);
      translate([0, 0, cap_t + 1 - 1.6]) cylinder(d1=2.8, d2=5.4, h=1.61);
    }
    // гравировка на лицевой панели
    translate([mod_w/2, dev_h/2 - neck_h/2 + 8, nd - 0.6])
      linear_extrude(1) text("DEYE WATCH", size=3.6, halign="center", font="Liberation Sans:style=Bold");
  }
}

// ---------------- защёлка DIN ----------------
// Профиль в плоскости (Y прибора, W): W=0 — полки рейки, W=4 — задняя стенка прибора, W<0 — к монтажной панели.
module clip_profile() {
  translate([rail_bot + 1.5, 0]) square([rail_top + 5.5 - rail_bot - 1.5, 4]);            // жёсткое основание
  translate([rail_top + 0.3, -2.6]) square([3.2, 2.7]);                                     // верхний крюк: стойка
  translate([rail_top - 3.0, -2.6]) square([6.5, 1.35]);                                    //   губа за полкой
  translate([rail_bot - 2.5, 2.6]) square([4.5, 1.4]);                                      // перемычка к пружине
  translate([rail_bot - 2.5, -7.0]) square([1.4, 11.0]);                                    // пружина (гнётся по Y)
  polygon([[rail_bot - 1.15, -1.25], [rail_bot + 1.5, -1.25],                               // нижняя губа
           [rail_bot - 0.6, -3.8], [rail_bot - 1.15, -3.8]]);                               //   с наклонной гранью
  translate([-2, -7.0]) square([rail_bot - 2.5 + 2 + 0.01, 1.4]);                           // рычаг под отвёртку
  translate([-2, -7.0]) square([3, 3.6]);
}
module clip() {
  difference() {
    rotate([90, 0, 90]) linear_extrude(clip_w) clip_profile();
    for (y=clip_screw_y) translate([clip_w/2, y, -1]) cylinder(d=2.0, h=6);
  }
}

// ---------------- рейка TS35/7.5 (для проверки) ----------------
module rail(len=60) {
  translate([-len/2 + mod_w/2, 0, 0]) rotate([90, 0, 90]) linear_extrude(len) {
    translate([rail_bot, -1]) square([5, 1]);
    translate([rail_top - 5, -1]) square([5, 1]);
    translate([rail_bot + 4, -7.5]) square([1, 6.5]);
    translate([rail_top - 5, -7.5]) square([1, 6.5]);
    translate([rail_bot + 4, -7.5]) square([27, 1]);
  }
}
module pcb_model() translate([pcb_x0, wall, zb]) {
  cube([pcb_w, pcb_l, pcb_t]);
  translate([3, 0, pcb_t]) cube([pcb_w - 6, 8, 3.3]);                       // USB-C ×2
  translate([(pcb_w - 18)/2, pcb_l - 26, pcb_t]) cube([18, 25.5, 5.7]);    // модуль + U.FL
}
module assembly(show_rail=true, show_pcb=true) {
  translate([0, 0, 4]) { body(); translate([0, 0, body_d]) cap(); if (show_pcb) color("green") pcb_model(); }
  translate([mod_w/2 - clip_w/2, 0, 0]) clip();
  if (show_rail) color("silver") rail();
}

if (part == "body") body();
else if (part == "cap") cap();
else if (part == "clip") rotate([0, -90, 0]) clip();
else if (part == "assembly") assembly();
