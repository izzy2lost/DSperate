# Google Play listing

Everything the Play Console asks for when setting up NinDS for testing.

| File | Use |
| --- | --- |
| `icon-512.png` | App icon (512×512) |
| `feature-graphic.png` | Feature graphic (1024×500) |

Screenshots still have to be taken on a device: at least 2 phone screenshots, plus 7" and 10" tablet ones if the app is offered on tablets.

## Building the upload

```sh
cd android
cp key.properties.example key.properties   # fill in the keystore path and passwords
./gradlew bundleRelease                     # app/build/outputs/bundle/release/app-release.aab
```

The bundle includes native debug symbols for Play's crash reports. Raise `versionCode` in `app/build.gradle.kts` for every upload.

## Store listing

**App name:** NinDS

**Short description** (80 characters max):

> Fast Nintendo DS emulator with high-resolution 3D, box art and touch controls.

**Full description:**

> NinDS is a fast, open-source Nintendo DS emulator for Android, built on the DSperate core: an ARM64 recompiler paired with accurate, melonDS-verified emulation.
>
> FEATURES
> • Plays .nds games and zipped games straight from the folder you choose
> • Library with banner icons or box art, downloaded on demand
> • GPU 3D through Vulkan, with up to 8x internal resolution, anti-aliasing and smooth textures
> • Screen filters: sharp, smooth, sharp bilinear, LCD grid and xBRZ upscaling
> • Stacked, side-by-side, hybrid and single-screen layouts for portrait and landscape
> • Save states with thumbnails, plus an automatic resume slot
> • Fast forward and frameskip
> • Action Replay cheats, typed in or picked from a usrcheat.dat database
> • Optional online play through Wiimmfi
> • On-screen controls with haptics, plus gamepad and keyboard mapping
> • Light and dark themes
>
> No BIOS dumps needed: the built-in FreeBIOS starts games directly. Your own BIOS and firmware dumps are optional and unlock the DS system menu.
>
> NinDS does not include any games, BIOS files or firmware. You must provide your own legally obtained game files.
>
> NinDS is free software under the GNU GPL v3.0. Source code: https://github.com/izzy2lost/DSperate
>
> NinDS is not affiliated with or endorsed by Nintendo. Nintendo DS is a trademark of Nintendo.

**Category:** Games › Arcade (or Tools)
**Contact email:** izzynochill@gmail.com
**Website:** https://www.izzy2lost.com/ninds
**Privacy policy:** https://www.izzy2lost.com/ninds-privacy

## App content answers

- **Ads:** No ads.
- **App access:** All functionality is available without special access.
- **Target audience:** 13 and over (the privacy policy says the app is not directed to children under 13).
- **Content rating:** Questionnaire category "Utility, Productivity, Communication, or Other" or "Game". No violence, sexuality, gambling, or user-generated content of its own. Answer **yes** to "users can interact or exchange information" only if online play (Wiimmfi) should count; it is off by default and runs inside the games themselves.
- **News app:** No.
- **Government app / financial features / health:** No.

### Data safety

- **Does the app collect or share any of the required user data types?** No.
  - The app has no accounts, analytics, ads or crash reporting.
  - Box art downloads are plain file requests to GitHub, carrying no user data beyond the game codes in the file names.
  - Online play sends game traffic to Wiimmfi only when the user turns it on. Google's guidance treats data a user directs to a service through the app's core function as exempt from "sharing".
- **Is data encrypted in transit?** Box art: yes (HTTPS). Online play uses the games' own DS-era protocols, so if Play asks, answer that not all data is encrypted, or answer "No" to collection.
- **Can users request data deletion?** Not applicable (nothing is collected). Local data goes away when the app's storage is cleared.

## Permissions

| Permission | Why |
| --- | --- |
| `INTERNET` | Box art downloads; optional Wiimmfi online play |
| `VIBRATE` | Haptic feedback on the on-screen controls |

No storage permissions: games, BIOS files and saves are reached through the Storage Access Framework.
