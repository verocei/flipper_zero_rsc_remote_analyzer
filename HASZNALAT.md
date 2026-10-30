# RSC Remote Analyzer – használati útmutató

Flipper Zero alkalmazás (Momentum firmware) ismeretlen kapu-, sorompó- és egyéb rádiós távirányítók
vizsgálatára. Elég megnyomni és nyomva tartani a távirányító gombját: a Flipper maga keresi meg a
frekvenciát, végigpróbálja a négy vételi beállítást, és megpróbálja felismerni a protokollt.

Az alkalmazás **csak vesz, soha nem ad**. Helye a Flipperen: *Apps › Sub-GHz › RSC Remote Analyzer*.

**A felület telepítés után angol.** Magyarra a *Settings › Language/Nyelv* sorban lehet váltani (a sor
neve mindkét nyelven ugyanez, hogy bármelyik nyelvről megtalálható legyen). Az útmutató a magyar
feliratokat idézi; az angol változat a [USAGE.md](USAGE.md).

> A Flipper beépített betűkészlete nem tud ékezetes betűt, ezért a képernyőn ékezet nélkül áll minden
> magyar szöveg („Automatikus vizsgalat”).

## Főképernyő

- **Automatikus vizsgálat** (a nagy gomb) – frekvencia, moduláció és protokoll magától.
- **Kézi vizsgálat** – te választod a frekvenciát, a moduláció lehet *Auto* (mind a négy) vagy egy
  kiválasztott; az „Inditas (OK)” sorral indul.
- **Mentett vizsgálatok** – a korábbi mentések jelentései.
- **Beállítások** – lásd lent.

Fel/le választ, OK indít, Back kilép.

## Automatikus vizsgálat lépésről lépésre

1. Indítsd el, tartsd a távirányítót a Flipper közelébe (10–50 cm), és **nyomd meg és tartsd
   nyomva** a gombját.
2. **Frekvencia keresése:** a Flipper végigpásztázza a Sub-GHz frekvencialistát (a
   `subghz/assets/setting_user` szerint), a legerősebb jel körül keskeny sávval pontosít, és a
   közeli szabványos frekvenciára kerekít (pl. 433,92 MHz). A nagyon közel tartott távirányító a dupla
   frekvencián is hallatszik (433,92 → kb. 868 MHz); a program ezt felismeri, és az alapfrekvenciát veszi.
3. **Moduláció:** sorban kipróbálja az **AM270, AM650, FM238, FM476** beállítást, mindegyikkel a
   firmware összes dekóderével. A képernyőn a négy négyzet közül a kitöltött az éppen próbált; alatta
   **teli csík** = dekódolt jelet, **pontozott vonal** = vizsgálva, de nem dekódolt.
4. Ha egy beállítás két egyforma dekódolást kap, azonnal a következőre lép; különben az ablak
   végéig (Beállítások › Ablak/preset) hallgat.
5. **Ha a távirányító közben abbahagyja az adást**, a képernyő alján megjelenik:
   **„Nyomd meg ujra a taviranyito gombjat!”** (és rezeg). Nyomd meg újra – a vizsgálat ott
   folytatódik, ahol abbamaradt, a korábbi eredmények megmaradnak.
6. Ha a talált frekvencián 5 másodpercig semmi nem jön, a Flipper újra megkeresi a frekvenciát
   („Tartsd nyomva a gombot!”); a már vizsgált beállítások és a dekódolások megmaradnak. Ha ezúttal más
   frekvenciát talál, a beállításokat az új frekvencián elölről vizsgálja.
7. **OK** bármikor: kész, az eddigiekből mutatja az eredményt. **Back**: megszakítás.

## Eredmény

- **Ismert protokoll:** név, fix kódos / ugrókódos, frekvencia, a legvalószínűbb moduláció,
  megbízhatóság (*magas*: legalább 3 dekódolás vagy két beállításon is; *jó*: 2; *alacsony*: 1), és
  a kód első sora.
- **„Ismeretlen protokoll – további vizsgálat szükséges.”** – jel volt, de egyik dekóder sem
  ismerte fel. Ilyenkor a moduláció csak *valószínű* (az impulzusok mintájából), és a program nem
  mondja meg, hogy fix vagy ugrókódos – nem is tudhatja.
- **„Nem érkezett jel”** – nem jött adás a vizsgálat alatt.

Gombok: **Új** (újabb vizsgálat ugyanígy), **Mentés**, **Több** (Részletes adatok, Modulációk –
mind a négy beállítás eredménye –, Kompatibilitás).

## Fix kód és ugrókód

**Honnan tudja?** Nem a jelből méri, hanem a felismert protokollból: a firmware minden dekódere
tudja a saját protokolljáról, hogy fix vagy ugrókódos (a protokoll típusa: *Static* / *Dynamic*). Ezért
egyetlen sikeres dekódolás elég hozzá. Ismeretlen protokollnál nincs mire támaszkodni, ilyenkor nem is
állít semmit.

- **Fix kódos** protokoll: minden gombnyomás ugyanazt küldi. A mentett `.sub` fájl a Sub-GHz
  alkalmazásban (*Saved*) megnyitható. Küldeni csak saját rendszeren, a helyi szabályok szerint
  szabad.
- **Ugrókódos** protokoll: minden gombnyomás új kódot küld, ezért egyszerű rögzítéssel és
  visszajátszással **nem másolható**; új távirányítót a vevőegység kézikönyve szerint, a vevővel
  párosítva lehet használni. Ilyenkor a mentés **csak a jelentést** írja ki, `.sub` fájlt nem.
- A *Részletes adatok* „Azonos ismétlés” és „Eltérő kód” száma segít: ha két gombnyomás más kódot
  ad, az ugrókódra utal (vagy két külön gombra).

## Mentés

A *Mentés* gomb után nevet adhatsz (alapból hónap, nap, óra, perc: pl. `RA_1030_0915`). Ami az SD-kártyára kerül:

| Fájl | Mikor |
| --- | --- |
| `apps_data/rsc_remote_analyzer/reports/<név>.txt` – a jelentés (idő, frekvencia, moduláció, protokoll, a négy beállítás eredménye, a dekódolt adat) | mindig |
| `subghz/rsc_remote_analyzer/<név>.sub` – Flipper SubGhz Key File | felismert fix kódos protokollnál |
| `subghz/rsc_remote_analyzer/<név>.sub` – Flipper SubGhz RAW File (elemzéshez) | ismeretlen protokollnál, ha volt felvétel |

A `.sub` fájlok a Flipper saját formátumában vannak, a Sub-GHz alkalmazás és a ProtoView is
megnyitja őket. A jelentések a főképernyő *Mentett vizsgálatok* menüjében olvashatók. A jelentés
kulcsai és értékei (pl. `Confidence: high`) a felület nyelvétől függetlenül angolok, hogy minden mentés
egyforma szerkezetű legyen.

## Beállítások

| Beállítás | Jelentés |
| --- | --- |
| Language/Nyelv | **English** (alap) vagy Magyar |
| Érzékenység | mitől számít jelnek: Alacsony −65 dBm, **Normál −75 dBm**, Magas −85 dBm (zajos helyen az Alacsony, gyenge távirányítónál a Magas) |
| Ablak/preset | meddig hallgat egy beállítással: Rövid 0,6 s, **Normál 1 s**, Hosszú 1,6 s |
| Mind a 4 preset | **Igen**: mindig mind a négyet kipróbálja; Nem: az első biztos dekódolásnál megáll |
| Visszajelzés | rezgés és LED (dekódolás, újabb gombnyomás kérése, eredmény) |
| Hibanapló (Debug log) | **Ki**; Be: minden vizsgálat naplót ír az `apps_data/rsc_remote_analyzer/debug_logs/` mappába (a vizsgálat ideje, a jelszintek és a lépések az utolsó kb. 20 másodpercből); az utolsó 5 vizsgálaté marad meg, a `debug_log_1.txt` a legújabb – hiba bejelentéséhez |

Az újabb gombnyomás attól számít jelnek, hogy a frekvenciakeresésnél mért szint alatt legfeljebb 10 dB-lel
van, de soha nem szigorúbb az Érzékenység beállításánál, és legfeljebb 10 dB-lel enyhébb nála (Normál:
−75 és −85 dBm között).

## Korlátok

- A Flipper CC1101 rádiója egyszerre egy modulációt tud venni, ezért a beállítások egymás után
  következnek: a rövid ideig adó távirányítónál több gombnyomás kell.
- A frekvencia és a moduláció megtalálása nem jelenti, hogy a protokoll is felismerhető.
- Az első változat a belső rádiót használja (külső CC1101 modul nincs támogatva).
- Csak azokat a protokollokat ismeri fel, amelyekhez a Momentum firmware-ben dekóder van.
