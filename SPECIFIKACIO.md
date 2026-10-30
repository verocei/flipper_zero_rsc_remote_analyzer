# RSC Remote Analyzer – fejlesztési specifikáció

Automatikus rádiós távirányító-felismerő alkalmazás. Célplatform: Flipper Zero, Momentum firmware;
natív alkalmazás (FAP). Munkanév a specifikációban: *Remote Analyzer*; a program neve
**RSC Remote Analyzer** (2026-10-30).

**Alapelv:** a felhasználó ne rádiótechnikai beállításokat keressen, hanem egyszerűen megnyomja a
távirányítót, a Flipper pedig automatikusan elvégzi a lehetséges vizsgálatokat.

## 1. Cél

Ismeretlen rádiófrekvenciás kapu-, sorompó- és egyéb távirányítók jelének automatikus vizsgálata
úgy, hogy a felhasználónak ne kelljen kézzel kiválasztania a frekvenciát, a modulációt és a
protokollt. Ideális esetben egyetlen hosszú gombnyomás elég. Az app megpróbálja megállapítani, hogy
a távirányító kompatibilis-e a Flipper Zero funkcióival.

## 2. Alapfunkciók

**2.1 Automatikus frekvenciafelismerés:** a beérkező jelek érzékelése, a frekvencia és a jelerősség
meghatározása, a megtalált frekvencia kiválasztása és kijelzése – a CC1101 képességein és az
engedélyezett tartományokon belül.

**2.2 Automatikus modulációfelismerés:** a frekvencia után az AM270, AM650, FM238 és FM476 vételi
beállítás automatikus váltogatása (a szám a szűrősávszélesség); annak ellenőrzése, melyikkel fogható
értelmezhető jel; a dekódolt adat előnyben; a legvalószínűbb moduláció kijelzése. A Momentum
„Hopping” funkciója csak frekvenciát vált, a modulációváltást külön kell megoldani.

## 3. Egyetlen hosszú gombnyomás

Folyamat: vizsgálat indítása → a gomb nyomva tartása → frekvencia → modulációk váltogatása →
dekódolás → eredmény. A távirányítók eltérnek: van, amelyik a nyomva tartás alatt ismétel, van,
amelyik csak néhányszor ad. Ha a jeladás megszűnik: **„Nyomd meg újra a távirányító gombját!”**, és
a keresés a korábbi eredményeket megtartva folytatódik. A CC1101 egyszerre egy modulációt vesz, a
beállításokat egymás után kell vizsgálni.

## 4. Protokollfelismerés

Ahol lehet: protokoll neve, frekvencia, moduláció, dekódolt azonosító adatok, fix/ugrókódos
jellemzők, a felismerés megbízhatósága – a Momentum meglévő Sub-GHz dekódereivel. Ismeretlen
protokollnál nem szabad fix vagy ugrókódos működést feltételezni; kijelzés: **„Ismeretlen
protokoll – további vizsgálat szükséges.”**

## 5. Fix kód és ugrókód

- **Fix kód:** azonosítás, a dekódolt adatok megjelenítése, a támogatott jelek mentése, a saját
  rendszeren történő emuláció lehetőségének jelzése.
- **Ugrókód:** a protokoll felismerése (ha támogatott), az ugrókódos működés jelzése, a vevővel való
  párosítás szükségességének megjelenítése. Az app ne állítsa, hogy ugrókódos jel egyszerű
  rögzítéssel és visszajátszással másolható.

## 6. Felület

- Főképernyő: REMOTE ANALYZER – Automatikus vizsgálat (nagy gomb), Kézi vizsgálat, Mentett
  vizsgálatok, Beállítások.
- Vizsgálat közben: frekvencia, moduláció (a próbált is), protokoll, jelerősség grafikusan, állapot.
- Eredmény: összefoglaló; Mentés, Új vizsgálat, Részletes adatok, Kompatibilitási információk.

## 7. Mentés

SD-kártyára: időpont, frekvencia, moduláció, protokoll, az azonosítás eredménye, a felhasználó által
megadott név, szükség esetén a jel adatai. Lehetőleg a Flipper `.sub` formátumában; az elemzés
metaadatai külön fájlba.

## 8. Technikai követelmények

A Momentum meglévő komponenseinek (Frequency Analyzer, Sub-GHz Read, Read RAW, CC1101-vezérlés,
protokolldekóderek) felhasználása; a cél az automatizálás, nem az újrafejlesztés. A protokollfelismerés
a firmware ténylegesen támogatott dekódereivel működjön; ami külső appból nem érhető el, arra más
megoldás kell.

## 9. Áttekintendő alkalmazások

RF Survey, Radio Scanner, Sub Analyzer, ProtoView – kiindulási ötletnek; teljes automatikus
moduláció- és protokollfelismerést egyik sem ad.

## 10. Ütemezés

1. **Automatikus rádiójel-elemzés:** frekvencia, a négy preset végigpróbálása, jelérzékelés,
   protokollazonosítás, eredmény kijelzése.
2. **Intelligens keresés:** egy hosszú gombnyomás, a megszűnő jeladás felismerése, új gombnyomás
   kérése, a korábbi eredmények megtartása, a modulációkeresés optimalizálása.
3. **Továbbfejlesztés:** fix/ugrókód felismerésének javítása, mentés, kompatibilitási információk,
   részletes jelelemzés, további protokollok.

## 11. Elfogadási feltételek

- A modulációt nem kell kézzel megadni; az app végigpróbálja a négy presetet.
- Felismer legalább egy ismert AM és egy ismert FM távirányítót.
- A támogatott protokollokat azonosítja; a puszta jelfogást nem keveri a protokollfelismeréssel.
- Túl rövid adásnál újabb gombnyomást kér.
- Az eredményt egyértelműen mutatja; Momentum alatt stabil.

## 12. Megjegyzések

A frekvencia és a moduláció megtalálása nem jelenti, hogy a protokoll is felismerhető. A cél a
lehető legtöbb információ, nem minden távirányító garantált másolása. Mentés és emuláció csak a
támogatott protokollokkal, a készülék rádiós és a jogszabályi korlátain belül.

---

## Megvalósítás – 0.1 (2026-10-30)

Az 1. és 2. ütem, a 3. ütemből a mentés és a kompatibilitási információ elkészült. Valódi
távirányítóval kipróbálva 2026-10-30-án: működik; a küszöbök és az időzítések hangolása a további
próbák alapján.

| Pont | Megoldás |
| --- | --- |
| 2.1 | RSSI-pásztázás a Sub-GHz frekvencialistán (OOK 650 kHz), megerősítő mérés, majd 58 kHz-es szűrővel ±300 kHz finom keresés; a legközelebbi listás frekvenciára kerekítve (±75 kHz). |
| 2.2 | A négy preset egymás után, mindegyik az összes dekóderrel; a nyertes a legtöbb dekódolást adó preset. Dekódolás nélkül csak „valószínű” moduláció az impulzusmintából. |
| 3 | Jelfigyelés 5 ms-onként; 350 ms csend = vége az adásnak; 400 ms várakozás után „Nyomd meg ujra a taviranyito gombjat!” + rezgés; a már vizsgált presetek és dekódolások megmaradnak. |
| 4 | Protokollnév, a dekóder szövege, megbízhatóság (dekódolások száma, presetek száma). |
| 5 | A protokoll típusa (Static / Dynamic) dönt; ugrókódnál párosítási tájékoztatás, `.sub` nem készül. |
| 6 | Saját főképernyő nagy gombbal; vizsgálati képernyő négy preset-csempével és jelerősség-sávval; eredmény Új / Mentés / Több gombbal. |
| 7 | Jelentés `.txt` + Flipper `.sub` (Key File a fix kódú protokollnál, RAW File ismeretlennél). |

**Eltérések, döntések:**

- A felület ékezet nélküli, mert a Flipper betűkészlete nem tud ékezetes betűt.
- Az app csak vesz; „ismétlés” = új vizsgálat, nem adás. Küldeni a Sub-GHz alkalmazásból lehet.
- Az első változat csak a belső CC1101-et használja.
