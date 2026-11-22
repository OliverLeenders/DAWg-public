# Design: pitch correction and note editing of monophonic audio

Status: **accepted** (owner, 2026-11-16), not built. The decisions are numbered PC1–PC14 and summarised in section 12.
The owner's answers are in section 11. Section 10 lists the risks.

The owner's answers in short:

* Build it next.
* TD-PSOLA, DAWg's own, as the shifter (Rubber Band's real-time mode was tried first and sounded too artificial).
* Edits per pool file, and timing through markers.
* A Pitch view in the audio editor, and no automatic correction.
* Tension-point note shapes, tilted by moving nodes.
* Two render stages for stretched audio.
* The autotune node later.
* **A neural pitch detector from the start** rather than pYIN, which changes section 3.2 and adds a dependency.

The question: should DAWg get a built-in pitch editor in the style of Cubase's VariAudio or Melodyne (melodic mode),
processed offline the way time stretching is, and if so, how big is it and what does it need?

**Short answer: yes, it is feasible, mostly because time stretching already built the hard infrastructure.** A
monophonic MVP is about **7–8 weeks** at the pace of the earlier phases, in five steps, including the neural detector.
It covers:

* detecting notes and showing them as blobs;
* dragging their pitch, splitting and merging them;
* **editing each note's shape** (onset scoop, drift, glide out) as a curve in DAWg's own curve model, made of its new
  smooth segments, with VariAudio-style tilting around movable pivots;
* snapping to the scale, vibrato control, and offline rendering.

Polyphonic editing (Melodyne's DNA) is out of scope (section 9). A real-time "autotune" effect node is a separate,
smaller feature (about 1–1.5 weeks) and should get its own design (section 6).

The design keeps **detection** and **editing** apart:

* Detection (section 3) is a pure analysis of the audio. It knows nothing of the project, clips or edits, and its
  result is cached, never saved.
* Editing (section 4) starts from a copy of what detection found and from then on is the user's data. Detection never
  runs again unless the user asks for it ("Re-detect").

## 1. Goals and non-goals

Goals:

* **Monophonic audio only:** vocals, solo instruments (bass, sax, lead guitar lines), one voice at a time.
* **Analysis on demand.** Opening a clip in the pitch view finds its pitch curve and splits it into notes ("blobs").
  Nothing is analysed or corrected automatically, as with timing (time-stretching.md, TS11).
* **Per-note pitch edits:**
  * move a note's pitch centre (snapped to semitones or free in cents);
  * reshape it: a smooth curve through the note's slow pitch movement, edited with nodes like automation, and tilted
    around movable pivots as in VariAudio;
  * scale its vibrato.
* **Per-note timing edits:** move a note's start and end, which stretches the audio around it. This reuses the timing
  markers (TS3, TS13).
* **Correction tools:** snap pitch centres to the semitone or to the project's key with an amount, and scale vibrato
  over a selection.
* **Non-destructive.** The edits are data in the project. The audio file is never changed; the result is rendered
  offline into a cache and played through the existing render path, as stretched audio is (TS7).
* **Exports are final quality,** never a preview (TS10).

Non-goals:

* **Polyphonic editing** (chords, piano, guitar strums, mixed material). See section 9.
* **Real-time correction on live input.** The optional autotune node (section 6) is a separate effect.
* **Formant editing as a creative tool** beyond one per-note formant offset later (step 6).
* **Sibilance, breath and level editing per note** beyond a note gain, later.
* **Pitch-to-MIDI with edits.** "Melody to MIDI" (audio-features.md, AU4) stays as it is, though it gains the better
  detector.

## 2. What exists today

### Offline time stretching (phase 18, time-stretching.md)

* **Library:** Rubber Band 4.0.0, single-file build, pulled by CPM (`cmake/Dependencies.cmake:58-62`), GPL-2.0-or-later
  used under GPLv3 inside AGPL DAWg (TS1, `README.md` licence section).
* **The interface:** `Stretcher` (`engine/include/dawg/Stretcher.h:52`) renders a buffer with a `StretchSpec`
  (`Stretcher.h:16`): output length, key frames (input frame → output frame), **one constant pitch scale** and a
  formants flag. `makeRubberBandStretcher()` (`Stretcher.h:81`) is the only implementation.
* **The Rubber Band call** (`engine/src/RubberBandStretcher.cpp:34-60`) uses the **offline** mode with R3
  ("finer"), formants preserved, high-quality pitch, channels together, no threads of its own
  (`RubberBandStretcher.cpp:90-92`). It sets `setTimeRatio`, `setPitchScale` and `setKeyFrameMap`, then studies and
  processes the whole input.
  * In offline mode the pitch scale is fixed for the whole run: Rubber Band's documentation says it may not be
    changed after `study()` or `process()`.
  * **So today's stretcher cannot follow a pitch curve.**
* **The renderer:** `StretchRenderer` (`engine/include/dawg/StretchRenderer.h:78`) runs jobs (`StretchJob`,
  `StretchRenderer.h:22`) on worker threads, nearest the playhead first, and keeps float WAV renders in
  `<app data>/Cache/stretch` under an LRU size limit.
  * A job's key hashes the file identity, the input range, the key frames, the output length, the pitch and the
    stretcher identity (`StretchRenderer.cpp:71`).
  * `renderOne` (`StretchRenderer.cpp:359`) does one job; `renderNow` (`StretchRenderer.cpp:413`) is the synchronous
    path export uses.
  * The Sampler already reuses it for its zone renders (`samplerRenderJob`, `StretchRenderer.h:54`).
* **How clips reference processed audio:** a clip never points at a render.
  * `compileAudio` (`engine/src/AudioPlayer.cpp:761`) compiles each musical clip into *pieces* (`CompiledAudioPiece`,
    `engine/include/dawg/AudioPlayer.h:22`): the file itself where nothing changes, a render where it does, or a
    repitch preview while that render is pending (`AudioPlayer.cpp:660-682`).
  * Pieces joining a render get a small correlation-matched offset, because crossfading two phase-vocoder renders of
    tonal audio cancels (time-stretching.md, section 11, "Joins").
* **The model:** `FileTiming` (`engine/include/dawg/TimingMap.h:55`) lives on the pool file (`AudioFile`,
  `engine/include/dawg/Model.h:1628`), shared by every clip of the file. `NoteClip::transpose` (`Model.h:205`) is the
  one per-clip pitch value. "Make copy" (`copyPoolFile`) gives a clip its own pool entry.
* **The UI:** the audio editor window (`ui/src/audioeditor/AudioEditor.tsx`, drawing in
  `ui/src/audioeditor/draw.ts:47`), opened by double-clicking an audio clip, shows the waveform through the timing map,
  transients and markers. The `dawg_stretch` event tells the UI which clips are pending.
* **Measured speed:** R3 offline runs about **5.6× real time on one core** (time-stretching.md, section 11, step 2):
  a three-minute stereo take is 32 s on one core, a two-segment marker move 1.7 s.

### Clip operations (audio-features.md)

The audio clip operations that exist are:

* reverse (AU1), which writes a new file linked by `AudioFile::reverseOf` (`Model.h:1676`);
* normalise (AU2), slice to Sampler (AU3) and audio to MIDI (AU4);
* crossfades (AU5–AU8) and punch recording;
* freeze, bounce in place and render selection (AU14–AU17).

The app runs analysis on its audio job threads through `AudioLibrary::analyse` (`app/src/AudioLibrary.cpp:307`), and
`audio.toMidi` is bridged in `app/src/MainComponent.cpp:2312`.

### Pitch and analysis code already in the repo

* **YIN pitch detection:** `detectPitch` (`engine/include/dawg/AudioToMidi.h:40`, `engine/src/AudioToMidi.cpp:136`):
  * the mono sum averaged down to about 12 kHz, a 20 ms window and a 5.3 ms hop, 50 Hz–1.5 kHz
    (`AudioToMidi.cpp:17`);
  * a threshold of 0.15 (`AudioToMidi.cpp:19`) and parabolic interpolation;
  * output `PitchFrame { seconds, pitch, levelDb }`.
* **Note segmentation:** `pitchNotes` (`AudioToMidi.h:86`, `AudioToMidi.cpp:229`):
  * splits voiced runs where the pitch leaves the note's running key by 0.6 semitone for 40 ms;
  * tolerates two unvoiced frames;
  * gives each note a key, a bend curve and a level curve;
  * does **not** split a repeated note at an onset (AU4 notes).
* **Onsets:** `detectOnsets` (`engine/src/Onsets.cpp:116`), spectral flux refined to the attack, cached per file in
  `<app data>/Cache/onsets`.
* **Curve simplification:** `simplify` (Ramer–Douglas–Peucker, `engine/include/dawg/MidiInput.h:181`), used by the
  note recorder and by Melody to MIDI to thin pitch curves.
* **Scales:** `Project::key` (scale-highlighting.md, SH1) with the scales in `engine/include/dawg/Scales.h`, mirrored
  in `ui/src/music/scales.ts`. Snap-to-scale can use them as they are.
* **Piano-roll drawing** to borrow for the editor's key axis: `drawGrid`, `drawKeyboard` and the curve ribbons in
  `ui/src/pianoroll/draw.ts:33`, `:583`, `:341`.

### The tension-point curve model

Automation, note expression and the curve editors all share one curve model. The note shapes in this design use it,
with one addition made in step 1: smooth segments (section 3.5).

* **The types:**
  * `Curve` is `std::vector<CurvePoint>` (`engine/include/dawg/Curve.h:127`).
  * A `CurvePoint` (`Curve.h:33`) has an integer `time` (`Ticks`, `juce::int64`), a `value`, and the `shape` of the
    segment that leaves it (`Curve.h:16`): `linear`, `hold`, `power` (with a `tension`), `free` or, since step 1,
    `smooth`.
* **Free segments:**
  * A free segment has one **tension point** (`handleTime`, `handleValue`), as fractions 0..1 of the segment's
    rectangle.
  * `freeSegment` (`Curve.h:158`) turns it into a cubic Bezier whose two handles point at that point. In the middle it
    is a straight line; in a corner it is the sharpest bend.
* **Where it is used:** automation lanes store exactly this (`AutomationLane::points`, `Model.h:1372`), as do note
  expression curves.
* **Evaluation:** `evaluateCurve` (`Curve.h:144`, twin in `ui/src/pianoroll/geometry.ts:230`).
* **Splitting:** `splitCurve` (`Curve.h:183`) cuts a curve in two for split notes.
* **Editing:** the editing helpers in `ui/src/pianoroll/curveEditing.ts` serve both the piano roll's expression lanes
  and the arrangement's automation (`ui/src/arrangement/useAutomationEditing.ts`).
  * They include `addPoint`, `movePoints`, `bendSegmentToPointer`, `boxSelect` and `deletePoints`.
  * They work through an abstract `CurveMapping` (`curveEditing.ts:20`) from curve time and value to pixels.
* **Two properties matter for this design:**
  * A free segment never leaves its rectangle, so it runs from one node to the next without overshoot. Every peak and
    trough of a shape must be a node.
  * The tension point is relative to the rectangle, so moving a segment's two nodes keeps the look of its bend. A
    segment between two equal values is flat, whatever its tension point.

### Licences of the current dependencies

| Dependency | Licence | Notes |
|---|---|---|
| JUCE 9.0.4 | AGPL-3.0 (or commercial) | Sets DAWg's licence: AGPL-3.0-or-later. |
| Rubber Band 4.0.0 | GPL-2.0-or-later (or commercial) | Used under GPLv3 (TS1). |
| libfaust | LGPL-2.1-or-later | Linked statically. |
| Faust libraries | mostly LGPL with exceptions, MIT, STK-4.3 | Bundled next to the app. |
| LAME 3.100 | LGPL-2.0 | MP3 export. |
| CLAP 1.2.9 | MIT | Plugin hosting. |
| free-midi-chords | MIT | Chord data. |
| WebView2 SDK, Catch2, Monaco | Microsoft SDK licence, BSL-1.0, MIT | |

Because DAWg is already AGPL and already links GPL Rubber Band, **any GPL-compatible library is usable**. Only
proprietary or non-commercial licences are out.

The neural detector (section 3.2) would add:

| Dependency | Licence | Notes |
|---|---|---|
| ONNX Runtime (CPU) | MIT | Prebuilt Windows binaries, downloaded at configure time as the WebView2 SDK is. About 10–20 MB of DLL. |
| FCPE weights (CNChTu/FCPE) | MIT | Fast pitch model trained on singing. |
| CREPE weights (via torchcrepe) | MIT | The reference model; "tiny" to "full" sizes. |
| RMVPE weights (as shipped by RVC) | MIT with a research-use clause, reportedly | Not usable until its licence text is read; the clause is the likely blocker. |

## 3. Detection

**(PC1) Detection is a pure function of the audio and an analyser version.**

* It lives in new `engine/include/dawg/PitchAnalysis.h` and `engine/src/PitchAnalysis.cpp`.
* It depends only on JUCE's audio buffers, `Onsets.h`, `Curve.h`, `simplify` and the pitch model's runtime (3.2),
  never on `Model.h`.
* It replaces the internals of `detectPitch` and `pitchNotes`, so Melody to MIDI gets the better detector too.

```cpp
struct PitchTrack                        // per frame, fixed hop
{
    double hopSeconds;                   // the pitch model's hop, usually 10 ms (section 3.2)
    std::vector<float> pitch;            // MIDI semitones, NaN = unvoiced
    std::vector<float> voicedProb;       // 0..1
    std::vector<float> levelDb;
};

struct DetectedPitchNote
{
    juce::int64 leadStart;               // unvoiced lead-in (consonant, breath), moves with the note
    juce::int64 start, end;              // the voiced part, in file frames
    double centre;                       // perceived pitch, semitones
    Curve shape;                         // the slow pitch movement, relative to centre (section 3.5)
    double vibratoRate, vibratoDepth;    // Hz, cents (0 if none)
};

struct PitchAnalysis { int version; PitchTrack track; std::vector<DetectedPitchNote> notes; };

PitchAnalysis analysePitch (const juce::AudioBuffer<float>& audio, double sampleRate, const PitchModel& model,
                            const std::vector<Onset>* onsets = nullptr,      // the library's cached ones, if known
                            const std::function<bool()>& cancelled = {});

// For split and merge (section 4.2): measurements over a frame range of the cached track.
double measureCentre (const PitchTrack& track, juce::int64 start, juce::int64 end);
Curve fitShape (const PitchTrack& track, juce::int64 start, juce::int64 end, double centre);
```

Detection hands editing only these values and the two measuring functions. Nothing flows the other way.

### 3.1 Preparation

* The mono sum (first two channels, as AU4), then a 40 Hz high-pass so rumble doesn't fake low pitches.
* Resampled to 16 kHz, the rate both candidate models (3.2) were trained at.
* The level per frame (RMS in dB), for voicing and for weighting the centre.

### 3.2 Pitch per frame: a neural model

The options considered for the detector:

| Option | Licence | Quality | Effort |
|---|---|---|---|
| YIN as it is (`detectPitch`) | own | Good on clean tones; octave errors and dropouts on breathy, raspy or reverberant vocals, because each frame decides alone. | 0 |
| pYIN-style, own code | own | Clearly better than YIN: the Viterbi pass (3.3) removes most octave jumps and gives stable voicing. This is what Tony (C4DM) uses. | 2–3 days on top of YIN |
| pYIN Vamp plugin code (C4DM) | GPL | As above | Written against the Vamp SDK; rewriting is about as quick. |
| CREPE / RMVPE / FCPE-style neural pitch | MIT code; the weights' terms vary | Best on hard material (breathy, noisy, reverb). | 1–2 weeks: an inference runtime (ONNX Runtime, about 10–20 MB), model files, high CPU cost. |
| WORLD's Harvest | modified BSD | Very good for voice, slower than real time. | 2 days |

**(PC2) A neural pitch model from the start, run through ONNX Runtime.** The owner's choice (2026-11-16). It is
the most robust on hard material: breathy, raspy, reverberant and doubled vocals, which are exactly where a wrong
detection becomes a wrong note.

* **The model.** Two MIT-licensed candidates, chosen between in step 1 by accuracy on the owner's recordings and by
  speed (question 12):
  * **FCPE** (the recommended default): trained on singing, and made to be fast. The upstream project ships no ONNX
    file, so DAWg uses the unofficial export at huggingface.co/Scicom-intl/fcpe-onnx (MIT, the original weights). It
    has FCPE's mel front end inside the graph, so it reads 16 kHz audio and no front end in C++ has to match its
    training.
  * **CREPE** (tiny to full): the reference, from the ONNX exports of github.com/yqzhishen/onnxcrepe (MIT), with the
    frames' normalisation inside the graph. It reads 1024 raw samples at 16 kHz per frame. It is much slower in its
    larger sizes.
  * RMVPE is left out until its weights' research-use clause is checked (section 2, licences).
* **What it gives:** both models give, per frame, a **salience** over pitch bins: 360 bins of 20 cents for CREPE, from
  about 32.7 Hz to 1975 Hz. The pitch is the salience-weighted mean of the bins around the peak, which keeps a
  fraction of a bin. The peak's height is the confidence.
* **The hop:** the model's own (10 ms is usual; CREPE can run at 5 ms by taking more frames). `PitchTrack::hopSeconds`
  is that hop. Shapes and the correction curve are interpolated between frames, which is enough for scoops of
  30–100 ms; step 1 checks this on real scoops.
* **Running it** (`engine/src/PitchModel.cpp`, behind a small `PitchModel` interface so the model can be swapped):
  * ONNX Runtime with the CPU execution provider. One session per worker, one intra-op thread each, because the
    workers already run in parallel.
  * Frames in batches of about 256 per run. Long files are cut into 30 s chunks with 1 s of overlap, so memory stays
    small and the playhead's region can go first.
  * The model file ships with the app (`<app>/models/pitch/<name>.onnx`), with its licence beside it
    (`LICENSE-fcpe.txt`, from `app/models/pitch/`); ONNX Runtime's licence and third-party notices go to
    `<app>/licenses/onnxruntime/`. Its hash is part of the analyser version
    (PC1), so a new model invalidates the cached analyses, but never the stored edits (section 4.1).
* **Speed, measured (2026-11-16, Release, this machine, a 3-minute synthetic melody):**
  * One thread: FCPE about 45 s in all, CREPE tiny about 200 s, CREPE small about 480 s. CREPE's larger sizes are
    out on CPU; tiny is still more than four times slower than FCPE.
  * FCPE's model run on 30 s: 4.7 s on one thread, 2.6 s on two, 1.7 s on four, 1.2 s on eight. So
    `loadPitchModel` takes a thread count: one where analyses run in parallel, about four for the file the user
    waits for. That is about 10 s of model time for three minutes.
  * The rest per 30 s: onsets 1.4 s (the audio library caches them, so `analysePitch` takes them), Viterbi 0.3 s,
    notes and shapes 0.4 s.
  * Analysis runs once per file and is cached, so tens of seconds would be acceptable; a progress bar
    shows while it runs.
* **No fallback detector.** ONNX Runtime ships for every platform DAWg builds on, so the YIN path isn't kept alive
  for the editor. `detectPitch` stays for the autotune node (section 6), which needs low latency.

### 3.3 Viterbi smoothing

The model's salience still decides each frame on its own, so a smoothing pass removes the octave jumps and voicing
flicker that remain. This is the Viterbi decoding CREPE's own tools offer.

* **States:** the model's 360 pitch bins (voiced) and one unvoiced state.
* **Observations:** a voiced bin's is the frame's voicing probability times its salience relative to the peak. The
  voicing probability is the peak salience raised to the power that maps the model's threshold to 0.5 (0.33 for
  FCPE, 0.58–0.61 for CREPE, from the export's evaluation on PTDB-TUG), and 0.01 below −60 dBFS. The unvoiced state's
  is one minus the voicing probability.
* **Transitions:** from one frame to the next a pitch moves at most 12 bins (240 cents for CREPE), with a triangular
  weight, as in CREPE's own decoder. That follows a fast scoop; an octave jump has to pass through the unvoiced state.
  Switching between voiced and unvoiced has a probability of 0.01, so the voicing doesn't flicker.
* **Cost:** for three minutes at 10 ms, about 18 000 frames × 361 states × a window of 25 bins, well under a second
  in Release.
* **Result:** per frame, the weighted pitch around the best path's bin, so the fractional precision is kept.

### 3.4 Segmentation into notes

**(PC3) Segmentation extends today's rules.**

1. **Voiced regions:** runs of voiced frames, bridging unvoiced gaps of up to 15 ms.
2. **Splits inside a region,** at either of:
   * a pitch step on the curve median-filtered over 30 ms: more than 0.5 semitone, held for 40 ms. This is today's
     rule, which already keeps ±50 cents of vibrato as one note;
   * an onset (`detectOnsets`) with a level dip before it, which splits a repeated note. Today's `pitchNotes` misses
     these.
3. **Glides between notes** are split at their steepest point, so each note keeps its own scoop in and fall out.
   Steps are found on the pitch averaged over 150 ms (about one vibrato cycle), against the median of the note's last
   0.5 s, so vibrato and slow drift don't split a note. After the owner's recordings (2026-11-16) two clean-ups
   follow:
   * a piece shorter than 150 ms that moves more than a semitone is the bottom of a glide, not a note: it becomes the
     next note's scoop (or the previous one's fall);
   * touching notes whose centres are within half a semitone are one note, unless an onset split them.
4. **Unvoiced lead-ins:** an unvoiced stretch above −50 dBFS and shorter than 300 ms just before a note becomes that
   note's `leadStart`. It moves with the note in time but is never pitch-shifted.
5. **The centre:** the mean pitch weighted by level over the middle 70 % of the voiced part, leaving out the attack
   scoop and the release fall. That is close to what the ear hears as the note, unlike the median.
6. **Vibrato:** the deviation from the centre, band-passed to 4–8 Hz. Its autocorrelation gives the rate, and its RMS
   the depth. Below 15 cents of depth there is no vibrato.

### 3.5 The note's shape: fitting a smooth curve

**(PC4) Each note's slow pitch movement is fitted with a `Curve` of smooth segments, a shape added to DAWg's shared
curve model for this.**

* **What is fitted:** the deviation from the centre, `dev(t) = p(t) − centre`, without its vibrato. This is the
  note's **shape**: the onset scoop, the drift, and the glide out.
  * Where the note has vibrato, the vibrato is removed by taking the midline between the curve's upper and lower
    envelopes (spline through the peaks, spline through the troughs, averaged).
  * A low-pass filter isn't used, because it would also smear the scoops, which take only 30–100 ms.
  * Elsewhere a 90 ms median removes the wobble. A median keeps a monotonic scoop as it is, which a moving average
    wouldn't.
* **Time unit:** file frames from the note's start.
  * `CurvePoint::time` is an integer (`Ticks`, `juce::int64`) relative to a start, so frames fit without changing the
    type.
  * `evaluateCurve` takes a fractional time already.
  * Values are semitones relative to the centre.
* **Why not tension-point (`free`) segments.** Step 1 fitted those first. Each segment bends towards its own tension
  point, and nothing makes neighbouring segments meet at the same slope, so every node was a corner, while sung pitch
  is smooth; the fit also needed a grid search and Nelder–Mead per segment. A prototype on the owner's six takes
  (2026-11-16) showed smooth segments as accurate with about as many nodes (10–13 cents mean error at the same node
  budget) and without the corners, so the shapes use them.
* **Smooth segments** (`CurveShape::smooth`, `smoothSlope` in `Curve.h`, twin in `ui/src/pianoroll/geometry.ts`):
  * A run of consecutive smooth segments is one monotone cubic through its nodes (Fritsch–Carlson, as PCHIP). At an
    inner node the slope is the weighted harmonic mean of the neighbouring secants, and 0 at a peak or a dip; at the
    run's ends a three-point estimate kept to the data's shape.
  * The curve passes through every node and never beyond them, has no corners, and moving a node changes only its
    neighbouring segments. Turning points stay nodes, which suits editing: a peak is something to grab.
  * A lone smooth segment is a straight line, and a run ends where another shape begins, so mixing shapes stays
    local. Automation and note expression can use the shape too; existing curves are unchanged.
* **The fit:**
  1. **Nodes.** Ramer–Douglas–Peucker (`simplify`, `MidiInput.h:181`) with a tolerance of 25 cents, which places
     nodes at the turning points.
  2. **Values.** The node values are refined by least squares (coordinate descent: each node moves to the bottom of
     the parabola through the error at its value and 5 cents either side, four rounds), so the curve needn't pass
     through noisy samples.
  3. **Thinning.** Each inner node is removed in turn if the refitted curve without it stays within the tolerance.
     That repeats until no node can go.
  4. **Limits.** About 4 nodes per second, but at least 4 per note: a short note still needs nodes at its turning
     points. Over the budget the tolerance loosens (×1.5 a step), but never beyond 50 cents; past that the note keeps
     its extra nodes rather than miss a turn. (A budget of `ceil(4 × seconds)` gave a 0.24 s note two nodes, and its
     shape ran half a semitone off the sung pitch.)
  * **Tuned on the owner's free singing (2026-11-16).** At 15 cents, a 15 ms median and 6 nodes per second, held notes
    got 7–14 nodes that followed every wobble. At 25 cents, a 90 ms median and 4 per second, a held note's shape is
    its scoop, a gentle drift and its fall, and the wobble stays in the detail.
* **Expected result:** a held note with a scoop and a fall gives 3–5 nodes. A slow drifting note gives 2–4. Vibrato
  is never in the shape: it is detail (section 4.4).
* **Is it exact?** No, nor does it need to be. What the fit misses (up to the tolerance) stays in the detail, so an
  untouched shape reproduces the sung pitch exactly (section 4.4).

### 3.6 Running and caching

* **Cache:** `<app data>/Cache/pitch/<hash>.json`, keyed by the file identity (path, size, modification time, as for
  onsets). It holds what the bridge sends (`pitchAnalysisToVar`) with its analyser (`pitchAnalysisVersion` and the
  model's identity); another analyser redoes it. JSON rather than a binary format, because the bridge sends JSON
  anyway and a take's analysis is small (about 100 kB for three minutes).
* **When it runs:** on the audio job threads (`AudioLibrary::pitchAnalysis`, beside the onsets), on demand when the
  Pitch view first opens. Never on import. The model (`models/pitch/fcpe.onnx` next to the app) is loaded once, with
  four threads, and analyses run one at a time; the file's cached onsets are reused.
* **Bridge:** `audio.pitchAnalysis { fileId }` returns the notes and the curve thinned for drawing (every second
  frame). A `dawg_pitchAnalysis` event says when it is ready. A model that can't be loaded gives `{ error }`, shown
  in the view.

## 4. Editing

Editing only reads `PitchAnalysis`. It never runs detection, except through "Re-detect".

### 4.1 Model

**(PC5) Pitch edits live on the pool file, like the timing markers.**

* The owner's choice for markers (TS2) applies for the same reasons: takes and comped pieces of one recording share
  the file, and editing a take fixes every clip cut from it.
* "Make copy" (`copyPoolFile`) gives a clip its own entry and copies the pitch edits too.

```cpp
struct PitchNote
{
    ObjectId id;
    juce::int64 leadStart, start, end;  // file frames; split and merge edit these
    double detectedCentre;              // from detection, kept so the edit means the same after re-analysis
    double targetCentre;                // = detectedCentre when untouched
    Curve detectedShape;                // the fit from detection, frozen (relative to detectedCentre)
    Curve shape;                        // the user's shape (relative to targetCentre); = detectedShape when untouched
    std::vector<juce::int64> pivots;    // tilt pivots, frames from the note's start (section 4.5)
    double vibrato = 1.0;               // 0 = removed, 2 = doubled
    double transitionMs = 20.0;         // smoothing of the step between neighbouring notes
};

struct FilePitch { int analysisVersion = 0; std::vector<PitchNote> notes; };   // AudioFile::pitch
```

* **Stored in the project:** the notes, both shapes, the pivots and the parameters. The fitted shape is stored
  (frozen) rather than recomputed, so a later analyser version cannot change what an edit means.
* **Cached, never stored:** the pitch track. It is needed only to scale vibrato and to draw.
* **Size:** a three-minute vocal has about 300 notes with up to about 10 nodes per shape, so a few thousand curve
  points. That is well within the per-curve limit of 4096 (`maxCurvePoints`, `Model.h:2373`).
* A file without `pitch`, or whose notes are all untouched, renders nothing and plays bit-identical, as now.
* The file format gains `AudioFile::pitch` with the next version number. Only test projects exist, so no
  compatibility mode is needed.

### 4.2 Seeding, splitting and merging

* **Seeding:** `seedPitchNotes { fileId }` copies the detected notes the first time the pitch view opens: the centre
  and the shape go into both their detected and their user fields.
* **Re-detect:** `seedPitchNotes { fileId, replace: true }` asks first, then replaces all edits with fresh notes.
* **Split** at frame `s`:
  * both shapes are cut with `splitCurve` (`Curve.h:183`);
  * each half gets its own `detectedCentre` from `measureCentre`. Its curves' values are shifted by the old centre
    minus the new one, so the sung and the edited pitch stay where they were;
  * pivots go to the half they fall in.
* **Merge** of neighbours A and B:
  * the curves are concatenated, B's times offset by its distance from A's start and its values re-referenced to the
    merged centre;
  * the lead-in gap between them becomes part of the merged note. In that gap its shapes run smoothly from A's last
    node to B's first. (They first held A's last value, a `hold` segment; once a node was deleted that step spanned
    sung audio as a vertical jump, so since 2026-11-17 every pitch shape is one run of `rounded` segments,
    normalised wherever a shape is read. `rounded` is a Catmull-Rom spline rather than the monotone `smooth` cubic:
    the monotone curve has a zero slope at every peak and stops dead where a steep rise meets a flat stretch, which
    read as a corner; the rounded one may pass a node by up to about 15 % of the neighbouring step. A shape's first and last nodes sit on the note's edges, level; they move only up and down and
    can't be deleted, so the curve spans the note and meets the held values beyond it without a corner.)
* So neither split nor merge loses edits, and neither needs detection again, apart from `measureCentre` on the cached
  track. If the track isn't cached yet, the command waits for the analysis.

### 4.3 What the user's shape means

**(PC6) The correction is the difference between the user's note and the sung note.** For each frame `t` of a note:

```
p(t)        the sung pitch (from the track)
c, c'       detectedCentre, targetCentre
S(t)        evaluateCurve (detectedShape, t)
S'(t)       evaluateCurve (shape, t)
detail(t)   = p(t) − c − S(t)               // what the shape doesn't hold: vibrato and fine jitter
vib(t)      = bandpass_4-8Hz (detail)(t)    // its vibrato part

target(t)   = c' + S'(t) + detail(t) + vib(t)·(vibrato − 1)
Δ(t)        = target(t) − p(t)
            = (c' − c) + (S'(t) − S(t)) + vib(t)·(vibrato − 1)
```

* **The detail always rides on the user's shape.** Vibrato and the small human jitter stay, on top of whatever shape
  the user draws, which keeps an edited note sounding sung.
* **Without vibrato scaling, Δ depends only on project data:** two centres and two curves. The pitch track is needed
  only for `vib(t)`.
* **Unvoiced frames** (gaps and lead-ins) get `Δ = 0`, with 10 ms ramps at the edges of the voiced audio, so
  consonants are never shifted.
* **Between neighbouring notes,** `Δ` may step if their centres moved differently. It is crossfaded over
  `transitionMs` with a raised cosine centred on the boundary. The glide itself is shaped by the notes' own curves, so
  this only removes the step.
* **The ratio** is `r(t) = 2^(Δ(t)/12)`, sampled at the analysis hop.
* **One pure function, twice:** the evaluation lives in C++ (`PitchCurve.h`) for the render, with a TypeScript twin
  (`ui/src/audioeditor/pitchCurve.ts`) that draws the corrected pitch live during drags. Both are tested against the
  same table, as `TimingMap` is.

### 4.4 Editing the shape

**(PC7) The shape is edited with the automation editor's gestures and code.**

* `ui/src/pianoroll/curveEditing.ts` works through a `CurveMapping` (`curveEditing.ts:20`).
  * For a note, its time axis maps file frames from the note's start to pixels through the timing map, as the audio
    editor already draws.
  * Its value axis maps semitones relative to `targetCentre` to the key grid.
* **The same gestures as the automation editor:** Alt + click adds a node, dragging moves nodes, Delete removes
  nodes, and a box selects them. Smooth segments have no tension point to bend; switching a segment to `free` or
  `linear` (for a deliberate kink) makes it bendable as in automation.
  * Nodes are kept inside the note: `CurveMapping`'s `maxTime` is the note's length.
* **Snapping:** times snap to nothing by default, because a note's shape is about the audio, not the grid. Values snap
  to 10 cents, or to semitones with Ctrl inverted, as elsewhere.
* **Presets** for common jobs, each one command on the selection:
  * **Flatten onset:** sets the shape's first segment to the value of its second node, removing the scoop.
  * **Flatten release:** the same for the last segment.
  * There is no separate "reset shape": Reset (section 4.8, `resetPitchNotes`) puts a note back as sung, shape
    included.
  * **No straighten.** A straighten amount that scaled the inner nodes towards the centre (VariAudio's "Straighten
    Pitch") was built and removed on 2026-11-18: dragging nodes, tilting and flattening the ends already shape the
    drift, and a second, hidden scale on the shape made those edits harder to follow.
* **The vibrato handle:** a selected note shows a small bar below the middle of its bar. Dragging it up deepens the
  vibrato, down flattens it (80 px for 100 %, from 0 to 200 %), and double-clicking it puts it back as sung. With
  several notes selected, each moves from its own amount. The amounts show next to the handles while one is dragged,
  and on notes whose vibrato is changed. The side panel's Vibrato slider does the same for the selection, or for
  every note.
* **The centre follows the shape (as VariAudio does):** after a shape edit (a node drag, a tilt, a removed node,
  flatten onset or release) the note is recentred (`PitchNote::recentre`). Its centre moves by the mean of
  `shape − detectedShape` over the middle 70 % of the note, the part the analyser measures the centre on, and the
  shape moves the other way, so what plays doesn't change. The bar then sits at the pitch the note now has, and Snap
  works from there. A shape edited back to the sung one gives the sung centre. `setNoteShape` takes the centre its
  shape is relative to, so each step of a drag, which the UI computes from the drag's start, means the same however
  the earlier steps moved the centre.

### 4.5 Tilting around pivots (VariAudio style)

**(PC8) Pivots are positions on the shape; tilting moves the nodes on one side of a pivot along a line through it.**

* **Setting a pivot:** a click with the tilt tool on a note's shape sets a pivot there (`pivots`). If the shape has no
  node at that time, one is added with the curve's value there, so the curve doesn't change. A note may have several
  pivots; the first is in the middle by default.
* **Tilting:** dragging up or down on one side of a pivot tilts that side. Every node between the pivot and the next
  pivot (or the note's edge) on that side moves by `k · (t − pivot) / (edge − pivot)`. Here `k` is the drag in
  semitones, and the node at the edge moves by exactly `k`.
  * The pivot itself and the other side stay where they are.
  * With several pivots, each span between two pivots tilts on its own, which is VariAudio's tilting with several
    centre points.
* **What happens to the bends:**
  * Smooth segments recompute their slopes from the moved nodes, so the shape stays smooth and still passes through
    every node. (A free segment, if the user made one, keeps the look of its bend, its tension point being relative
    to its rectangle.)
  * This is not an exact shear of the old curve, which would need a straight line added to the curve. For the small
    angles tilting is used at, it is indistinguishable.
  * A segment whose two nodes end up at the same value is flat, so tilting a sloped segment to level straightens it.
    That is the expected result.
* **Drawing:** the pivot shows as a small diamond on the shape; the span being tilted is highlighted while dragging.
* **The data:** tilting changes `shape`'s nodes, and the pivots are only where to tilt from. There is no separate
  tilt state, so undo, copy and split need nothing special.

### 4.6 Centre, vibrato and timing

* **Dragging a blob up or down** moves `targetCentre`, snapping to semitones (to the key with Scale Lock on), with
  Ctrl for free cents. The shape moves with it, because it is relative to the centre.
* **Vibrato:** the `vibrato` amount on the selection, 0–200 %.
* **(PC9) Timing edits write timing markers.**
  * Dragging a note's left or right edge moves a timing marker (TS3) at its onset or end, which stretches the audio
    on either side through the existing `moveTimingMarker` (TS13).
  * This needs the file to be musical (TS2). On a one-shot file the pitch view offers "Make musical" and converts
    nothing by itself.
  * The note's frames don't change: timing lives in the timing map, pitch in the file's frames.

### 4.7 Correction tools

**(PC10) The tools are commands that set note parameters.** None needs new DSP.

* **Snap to semitone / to the key:** moves each selected note's `targetCentre` towards the nearest semitone, or the
  nearest note of `Project::key` (`Scales.h`), by an amount (0–100 %). Partial correction keeps some of the singer's
  intonation.
* **Vibrato depth** over a selection, or on a note's handle (4.4).
* **No quantising of note timing.** It was built (`quantiseNoteTiming`) and removed on 2026-11-18. The detected notes
  are the analysis' guesses, so snapping their starts to the grid put timing markers wherever the detector happened
  to cut a note. Timing is set in the Timing view's markers, or by dragging a note's edge (PC9), which snaps to the
  grid as the user places it.

### 4.8 Commands

Each command is one undo step; drags coalesce:

* `seedPitchNotes { fileId, replace? }`;
* `splitPitchNote { fileId, noteId, frame }` and `mergePitchNotes { fileId, noteIds }`;
* `setNotePitch { fileId, noteIds, target? | delta?, vibrato?, transitionMs? }`;
* `setNoteShape { fileId, noteId, shape, centre?, pivots? }`. The UI computes the new curve with `curveEditing.ts`
  and the tilt maths (one function shared by gestures and tests), as the piano roll does for expression; shape is
  relative to centre (the note's own without it), and the note is recentred afterwards (4.4);
* `shapeNotes { fileId, noteIds, op: flattenOnset | flattenRelease }`;
* `snapNotePitch { fileId, noteIds, mode: semitone | key, amount }`;
* `resetPitchNotes { fileId, noteIds? }`.

## 5. Rendering

### 5.1 The shifter

The engine must follow the time-varying ratio `r(t)` (section 4.3) without changing the length: every frame stays
where it was.

| Option | Licence | Quality on solo voice | Effort | Notes |
|---|---|---|---|---|
| **Rubber Band R3, real-time mode, driven offline** | GPL (already in) | Good for corrections up to about ±3 semitones with formants preserved; phasier beyond, and consonants smear slightly. | 3–4 days | `setPitchScale` may be called between `process()` calls in real-time mode, and `OptionPitchHighConsistency` is meant for continuously changing pitch. Run on a worker thread it is still faster than real time. Needs latency handling. |
| Rubber Band R3, offline mode, one render per note | GPL | Constant pitch per note only: no shape or vibrato edits, and a join at every note boundary. | 2 days | Not enough. |
| Signalsmith Stretch | MIT | Decent; its README says its formant correction is not as sharp as monophonic methods. | 3 days | A fallback if Rubber Band's licence ever mattered. |
| TD-PSOLA, own code | own | The most natural for small and medium shifts of a clean voice; formants are preserved by construction. Artefacts on breathy voice and wherever pitch marks are wrong. | 1.5–2 weeks | Needs pitch marks (glottal epochs). Very cheap to run. |
| WORLD vocoder | modified BSD | Flexible, but resynthesised recordings sound buzzy or "vocoded". | 1 week | Analysis is slow. |

**(PC11) TD-PSOLA, DAWg's own code, behind a `PitchShifter` interface** next to `Stretcher`. The owner's choice
after the shifter trial below (2026-11-16): Rubber Band's real-time mode sounded too artificial on edited notes, and
PSOLA more natural.

```cpp
class PitchShifter
{
public:
    virtual ~PitchShifter() = default;
    [[nodiscard]] virtual juce::String identity() const = 0;
    // As long as the input; output frame k is input frame k shifted. `ratio` is sampled every `hop` input frames.
    [[nodiscard]] virtual juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& input, double sampleRate,
                                                           const std::vector<float>& ratio, int hop,
                                                           const std::function<bool()>& cancelled) const = 0;
};

std::shared_ptr<const PitchShifter> makePsolaPitchShifter (PitchTrack track);   // the file's pitch
std::shared_ptr<const PitchShifter> makeRubberBandPitchShifter (bool finer, bool formants);   // for comparison
```

The PSOLA implementation (`engine/src/PsolaPitchShifter.cpp`):

1. **Pitch marks** one period apart where the track is voiced: the first of a voiced stretch on the highest peak of
   its first period, each later one on the highest peak within a quarter period of where one period on predicts. The
   peaks are those of the mono sum low-passed at 1 kHz (forwards and backwards, so they don't move). Where unvoiced,
   marks every 5 ms.
2. **Grains:** two periods of the input, Hann-windowed around each mark. They are never resampled, so the formants
   stay where they were.
3. **Synthesis:** synthesis marks start on the first analysis mark and step by the spacing to the next analysis mark
   divided by the ratio there (1 where unvoiced). Each takes the grain of the nearest analysis mark. Stepping by the
   marks' actual spacing rather than the nominal period makes a ratio of 1 put every grain back where it came from,
   so unedited audio comes out unchanged.
4. **Level:** the sum is divided by the sum of the windows, so grains overlapping more when the pitch rises don't
   raise the level.
5. **Splicing:** the same as any render: phrase edges sit in near-silence (5.2), so a 5 ms crossfade to the file is
   enough. PSOLA has no latency to trim.

**The trial (2026-11-16).** `PitchShifter` was built first with Rubber Band R3 in real-time mode
(`RubberBandPitchShifter.cpp`), then with PSOLA, and a hidden test case, `dawg_tests "[pitch-shift-tool]"`, renders
hand-made corrections of the owner's takes (`DAWG_SHIFT_INPUT`, `DAWG_SHIFT_OUTPUT`, `DAWG_SHIFT_VARIANTS` of
`r3-formants`, `r3` and `psola`, `DAWG_SHIFT_TRIALS`): unchanged, centres snapped, two notes moved by ±3 semitones,
vibrato removed (the fitted shape only), and hard-tuned flat. Each render was checked by detecting its pitch again.

* **Rubber Band, timing.** A ratio set before a block reaches the audio the shifter is synthesising then, not the
  block's: setting each block's own ratio put every change about 90 ms early. The lag is the start pad plus the start
  delay (measured at 44.1 and 96 kHz), so each block sets the ratio from that far back; a test checks that a step in
  the curve lands within 10 ms of its place. R2 ("faster") shifts too far in this mode (about 2.5 semitones for 2) and
  was left out.
* **Rubber Band, sound.** Unchanged audio passed through clean, but the owner found edited notes too artificial, with
  formants preserved or not (preserved was the better of the two).
* **PSOLA, measured.** Unchanged renders match the input (within 0.6 cents on re-detection; exactly, on a test tone).
  Hard-tuned renders sit 5–9 cents from the semitone on average, against 21–26 cents sung, as with Rubber Band.
  Changes land in place (the measured change follows the intended one at lag 0 with a slope of about 1.0).
* **PSOLA, sound.** The owner found it more natural than Rubber Band on the same edits.
* **Speed:** PSOLA about 450 times faster than real time on one thread; Rubber Band 6–9 times.
* **Still to watch:** PSOLA's known weak spots are breathy voice and wrong pitch marks. The prototype's marks are
  simple peak picking; if rough patches show up on real material, better epoch detection (for example on a
  glottal-pulse estimate) is the place to improve.

### 5.2 Phrases, cache and playback

**(PC12) Pitch renders are length-preserving renders of phrases, made before the stretch.**

* **The unit is a phrase:** notes chained with gaps of under 100 ms. Long phrases are cut at the quietest frame near
  8 s, as the stretch chunks are.
  * A phrase is rendered only if `|Δ| > 0.5 cent` somewhere in it. Other phrases play the file and stay
    bit-identical.
  * Editing one note re-renders only its phrase, typically 2–8 s of audio. At the measured speed that is about
    0.5–1.5 s of work.
* **The cache:** the `StretchRenderer` gains a pitch job kind, sharing its queue, folder, LRU limit and
  `dawg_stretch` events.
  * The key hashes the file identity, the phrase range, the Δ curve quantised to 0.1 cent, and the shifter's identity.
  * Hashing Δ rather than the parameters means edits that give the same correction share a render, and undo finds the
    earlier one.
* **Playback:** a `PitchedFileSource`, an `AudioSource` that reads the file but serves the phrase renders over
  rendered phrases.
  * **One-shot files, and musical segments at ratio 1:** clips play it through the existing file and render pieces
    (`AudioPlayer.h:22`). The player doesn't change.
  * **Stretched musical segments:** the stretch job reads its input from `PitchedFileSource`. Its key includes the
    keys of the phrase renders it overlaps.
* **Two stages, and why:** rendering pitch first (PSOLA) and then the existing stretch (Rubber Band) keeps each
  engine doing what it is good at. Audio that is both repitched and stretched goes through both (question 9).
* **While a phrase renders,** it plays uncorrected and the clip shows the pending bar. The UI never waits: renders are
  requested when a drag ends (after 150 ms without change), so a drag never floods the queue.
* **Export** runs `renderNow` for the pitch jobs first, then for the stretch jobs (TS10).

**As built (step 2, 2026-11-16):**

* **Pitch jobs** are `StretchJob`s with `pitch` set: a phrase plus 50 ms either side, rendered by PSOLA with the
  correction sampled every 5 ms. The key hashes the file identity, the range and the correction quantised to 0.1 cent.
  Phrases aren't cut at 8 s: PSOLA renders a long phrase in a fraction of a second.
* **Tracks:** the analysis cache keeps the full pitch track beside its JSON (`<hash>.track`), and the renderer reads
  it through `StretchRenderer::setPitchTracks`. A file not analysed yet plays uncorrected.
* **Playback:** `AudioSource::withOverlays` reads the file with each ready phrase render laid over its range;
  `compileAudio` gives it to the clip, so file pieces, repitch previews and join alignment all hear the correction.
  Pending phrases count in the clip's `pendingRenders`. No crossfade is needed: PSOLA reproduces unshifted audio
  exactly (where the ratio is 1, synthesis goes back onto the analysis marks), so a render's edges match the file.
* **Stretch renders** of a corrected file put the phrase renders into their input (rendering any that are missing),
  and their key includes the correction over their input.

## 6. The optional autotune effect node

**(PC13) A separate, real-time effect node,** not part of the editor, with its own design later:

* pitch detection on the input (YIN with a short window, about 20–40 ms of latency);
* a target from the project key or its own scale, a **retune speed** (instant for the hard-tuned effect, slow for
  transparent correction) and a humanise amount;
* the shifter: Rubber Band R3 in real time (formants preserved, but with latency that is high for monitoring), or a
  small own PSOLA or delay-line shifter (low latency, the classic hard-tuned sound). The node reports its latency for
  delay compensation;
* about **1–1.5 weeks.** It shares nothing with the editor's detector, which is too slow and needs too long a
  window for live input. It uses the existing YIN (`detectPitch`), adapted to stream.

## 7. Steps

1. **Detection and a read-only view (about 2.5 weeks).**
   * ONNX Runtime through CMake (a configure-time download, as the WebView2 SDK is) and the `PitchModel` interface.
   * FCPE and CREPE behind it (FCPE's front end is inside its export, so only CREPE's framing is C++). A benchmark on three
     minutes of vocals, and a comparison on the owner's recordings, pick the default model and its size (question
     12).
   * Viterbi smoothing, segmentation and the shape fit, with their cache (section 3).
   * The Pitch view in the audio editor, drawing the blobs, the sung curve and the fitted shapes, without editing.
   * This checks on real recordings that detection and the fits are good enough, before rendering is built. A
     hidden test case, `dawg_tests "[pitch-tool]"`, writes the analysis of a file or folder (`DAWG_PITCH_INPUT`) as
     JSON next to it, with the time taken, to look at it before the view exists. It's a test rather than a program
     under `tools/` so the engine isn't compiled a third time.
2. **Model and offline pitch rendering (about 1.5–2 weeks).**
   * `AudioFile::pitch`, seeding, `setNotePitch`, the file format (PC5, PC6).
   * The PSOLA `PitchShifter` (built in the trial), phrases, the cache, pending playback, export, and the chain
     into stretch renders (section 5).
   * **The fiddly part:** keys and invalidation.
   * A benchmark sets what the UI may promise.
3. **Note editing (about 1 week).** Dragging the centre, splitting and merging, selection, vibrato, audition, and the
   correction-curve twin for live drawing.
   * **As built (2026-11-16):** click or box-select notes, drag them up and down by semitones (Ctrl: freely), Alt +
     click to split (each half's centre measured on the analysis' pitch), Merge for selected neighbours, a vibrato
     amount, Snap to semitones and Reset for the selection (or every note). The corrected pitch is drawn live with
     `ui/src/audioeditor/pitchCurve.ts`, tested against the engine's table. Cutting a smooth shape moves the sung and
     edited pitch by a few cents near the cut, but never the correction. Clicking a note plays it from its lead-in to
     its end through the preview player (`audio.auditionNote`), corrected where its phrase render is ready; a
     checkbox in the side panel turns this off.
4. **Shape editing (about 1–1.5 weeks).** Nodes through `curveEditing.ts`, the tilt tool with
   pivots, flatten onset and release, and reset (PC7, PC8).
   * **As built (2026-11-16):** the Pitch view has one editing mode, in which what is under the pointer decides:
     a selected note's tilt handle, a shape's node, a note's edge (musical files), a note's bar, or empty space. (It
     first had Notes, Shape and Tilt tools; the owner found that each hid the others' gestures.) Nodes are edited with
     `curveEditing.ts`: drag in 10-cent steps or freely with Ctrl; double-clicking a note adds a node on its shape
     there, double-clicking a node removes it. A selected note shows tilt handles above its bar's corners; dragging
     one tilts that side around the pivot next to it (a note without pivots tilts around its middle), with the maths
     in `ui/src/audioeditor/pitchShapes.ts`. Shift + Alt + click sets a pivot. Between two pivots, the end nearer
     the drag moves. The side panel has
     flatten onset and release and reset shape, through `shapeNotes`; the UI sends finished curves with
     `setNoteShape`. Straightening was first a `shapeNotes` operation that scaled the shape's values, so it could
     not be turned back; since 2026-11-17 it is a note setting like vibrato (`PitchNote::straighten`, set through
     `setNotePitch`, ends kept), applied as the note plays (`heardShape`) and live on a slider. Edits that work on
     the shape's values (node drags, tilting, pivots, flattening, split and merge) bake it into the shape first. With only a pivot and an edge node, a tilted smooth span eases rather
     than ramps, as smooth segments do.
   * **The side panel (2026-11-18):** the Timing / Pitch switch is at the top of the side panel, not in the header,
     and everything below it belongs to the chosen view. The Pitch panel groups its tools as Pitch (snap to
     semitones or the key, an amount, "Snap …"), Shape (vibrato, flatten onset and release) and Notes (Merge,
     Reset). The tools act on the selection, or on every note with nothing selected, and the Snap, Merge and Reset
     buttons name their target ("Snap 3 notes", "Reset all 300 notes"). Vibrato shows "mixed" when the targets
     differ. Straightening moved from a slider to a handle on the note, and later the same day was removed: the
     handle now sets the vibrato, and shape edits recentre the note (section 4.4). The gesture list is folded
     away under "Gestures". A line over the canvas's bottom-left corner says what a press would do under the
     pointer.
5. **Correction tools and timing (about 1 week).** Snap to semitone or key with an amount, note timing through
   markers, and quantise timing (PC9, PC10).
   * **Since 2026-11-18:** quantise timing is gone (section 4.7).
   * **As built (2026-11-16):** `snapNotePitch` moves target centres towards the nearest semitone, or the nearest note
     of the project's key where the file's first clip starts, by an amount. `quantiseNoteTiming` puts a timing marker
     at each note's start with the content tick that lands it on the grid where the first clip plays it, by an
     amount, replacing a marker already at that frame. In the Notes tool, dragging a note's start or end edge on a
     musical file adds a marker there if needed and moves it (grid snapping, Ctrl off), as in the Timing view.
6. **Later, each optional.**
   * The autotune node (section 6), about 1–1.5 weeks.
   * Better pitch marks for PSOLA (epoch detection), if rough patches show up on real material.
   * A per-note formant offset and note gain, a few days.

**MVP = steps 1–5: about 7–8 weeks.** The neural detector adds about a week to step 1 over pYIN.

Steps 1 and the shifter part of step 2 are independent, so the shifter can be tried early: a `PitchShifter` with
hand-made correction curves on real takes (one note moved, a note straightened, vibrato removed). That shows the
shifter's quality before the model and UI are built on it. Done on 2026-11-16: Rubber Band's real-time mode sounded
too artificial, PSOLA was chosen (section 5.1). For comparison, time stretching was planned at about 16–17 days for its four
steps.

## 8. Tests

* **Detection:**
  * sine and sawtooth melodies within 5 cents;
  * a tone with a strong second harmonic that YIN gets an octave wrong and the model with Viterbi gets right;
  * voicing on a noise burst between notes;
  * the model's output on a fixed file is the same on every run, so the cache and tests can rely on it.
* **Segmentation:**
  * repeated notes split at onsets;
  * ±50-cent vibrato as one note with the right rate and depth;
  * a consonant lead-in attached to the next note;
  * the centre within 5 cents on a note with a scoop into it.
* **Shape fit:**
  * a synthetic note with a 1-semitone scoop, a slow 30-cent drift and a fall fits within 15 cents with at most
    5 nodes;
  * vibrato does not appear in the shape;
  * every turning point is a node.
* **Correction curve** (C++ and TS twin, same table):
  * untouched notes give `Δ = 0` everywhere;
  * a centre move gives a constant `Δ`;
  * a shape edit gives `S' − S`;
  * vibrato 0 removes the 4–8 Hz band;
  * unvoiced frames stay at 0;
  * transitions remove steps.
* **Tilt and split:**
  * a tilt leaves the pivot and the other side unchanged and moves the edge by the drag;
  * a node exactly at the pivot stays put;
  * split and merge keep `target(t)` the same at every frame.
* **Rendering:**
  * a sine melody with one note moved +2 semitones gets the new frequency (FFT peak) on that note only, and is
    bit-identical outside its phrase;
  * the output is as long as the input;
  * a note with a level shape and no vibrato stays within 5 cents of its centre;
  * a stretched musical file with an edited note has both the timing and the pitch;
  * export gives the same file twice.
* **Cache:** the key changes with each input and nothing else, and undo finds the earlier render.
* **Commands:** each of section 4.8, its undo, and "Make copy" copying the edits.
* **Speed:** a three-minute vocal fully corrected, and a one-note edit, on the Release build.
* **UI:** by hand over CDP on a real vocal recording, including tilting with two pivots.

## 9. Polyphonic editing: out of scope

**(PC14)** Melodyne's DNA edits single notes inside chords. It is out of scope for technical reasons:

* **Detection.**
  * Partials coincide in consonant intervals. In an octave, every partial of the upper note is also one of the lower
    note's. In a fifth, every third partial of the lower note meets every second of the upper.
  * Piano strings are inharmonic, guitars have weak fundamentals, and low notes a semitone apart need windows of
    hundreds of milliseconds to tell apart, which smears the onsets.
  * Even trained transcription models make frequent note errors on dense recordings.
* **Separation.**
  * Moving one note means taking its energy out of the mixture. Where two notes share a frequency bin, the bin holds
    one sum, and splitting it is underdetermined: every method guesses, from harmonic templates, sinusoidal tracking
    or a trained model.
  * Attacks, noise, pedal and room resonance have no pitch to assign them by.
* **Resynthesis.** An imperfect removal leaves a ghost at the old pitch, or holes in the other notes. Errors that
  cannot be heard in a solo voice are obvious inside a chord.
* **Dependencies.** Practical results today need neural separation: an inference runtime, models of tens to hundreds
  of megabytes and weights under licences to check, against DAWg's self-contained builds. An own sinusoidal-modelling
  approach would be a research project of months to years.
* **Value.** The common use, tuning a lead vocal or a solo line, is monophonic.

Monophonic audio has none of these problems: there is one pitch at a time, so all harmonic energy belongs to the one
note, unvoiced stretches are left alone, and the shifter works on the whole signal.

Patents (Celemony holds some on polyphonic note editing) are not the reason. They cover specific methods, not the
goal, and expire 20 years after filing. They would need checking before a polyphonic feature shipped.

## 10. Risks

* **Shifting quality (the main risk).**
  * PSOLA roughens breathy voice and wherever a pitch mark is wrong, and large shifts can sound hollow. Mitigations:
    shift only voiced stretches with a non-zero `Δ`, keep shifts small (correction rarely exceeds a semitone),
    improve the marks if needed, and keep the `PitchShifter` interface (Rubber Band stays behind it for comparison).
  * The trial chose PSOLA over Rubber Band's real-time mode by ear on the owner's takes (section 5.1); other voices
    may differ.
  * Audio that is both stretched and repitched (PC12) goes through two engines, which degrades a little more than
    one.
* **Detection.**
  * Doubled, distorted or reverberant vocals can still give wrong blobs, and correcting a wrong detection makes a wrong
    note. Mitigations: the model with Viterbi smoothing, editable splits, and step 1 checking real recordings first.
* **The neural model.**
  * The FCPE export is unofficial. Its card reports frame-level agreement with the PyTorch model; step 1's comparison
    on real recordings checks it, and CREPE is the fallback if it disappoints.
  * CPU cost: FCPE is about 15–25 s for three minutes on four threads (section 3.2); CREPE's sizes above tiny are
    too slow on CPU. Analysis runs once per file in the background.
  * The models were trained on data DAWg doesn't control, and may be weaker on instruments than on singing.
    Seen on the owner's recordings: a whistled melody (near-sine tones around 1.1–1.3 kHz; the owner confirmed it
    was whistled) is mostly unvoiced for FCPE, which also put one stretch an octave low; CREPE tiny finds more of it.
    Sung takes are clean with both, and the owner found the Pitch view right on them (2026-11-16). Whistling is outside
    what the editor is for, so FCPE stays the model.
* **The new dependency.**
  * ONNX Runtime adds 10–20 MB to the installer, a configure-time download, and a library to keep up to date. It is
    MIT-licensed, as are the FCPE and CREPE weights.
  * RMVPE's weights are reportedly MIT with a research-use clause, so RMVPE is not used until that is checked.
  * Each model's copyright notice goes into the README's third-party list.
* **Licence.** No new risk from Rubber Band: it is already in under GPLv3 (TS1), and DAWg is AGPL and never sold. A
  closed build would need Rubber Band's commercial licence, as stretching already does.
* **CPU and render times.**
  * At about 5.6× real time per core (the offline figure), a fully corrected three-minute vocal is about 30 s on one
    core, and a few seconds spread over the workers.
  * A one-note edit is one phrase, well under 2 s.
  * Real-time mode may be slower than offline mode; step 2's benchmark decides.
* **Chaining with stretch renders.** Keys and invalidation across two stages are where bugs will hide; step 2 budgets
  for it.
* **Scope creep towards Melodyne.** Polyphonic editing, sibilance tools and formant design are each projects of their
  own; section 9 and the non-goals keep them out.

## 11. The owner's answers (2026-11-16)

1. **Build it, and when?** Yes, next, with steps 1–5 as the MVP.
2. **Which shifting engine?** First Rubber Band R3 in real-time mode; after the shifter trial (2026-11-16) the owner
   chose TD-PSOLA, DAWg's own, behind `PitchShifter` (PC11).
3. **Pitch edits per pool file or per clip?** Per pool file, like timing markers, with "Make copy" (PC5).
4. **Timing edits through timing markers, so only on musical files?** Yes (PC9).
5. **Where does the editor live?** A Pitch view in the audio editor window.
6. **Any automatic correction on opening?** Never.
7. **Note shapes as curves in DAWg's model, edited like automation?** Yes (PC4, PC7). After step 1's prototype the
   owner chose smooth segments over tension-point ones (2026-11-16). The fit's tolerance and node limit were tuned on
   the owner's recordings: 25 cents and 4 nodes per second (section 3.5).
8. **Tilting:** by moving the nodes on one side of a pivot, not an exact shear (PC8).
9. **Two stages for audio that is both stretched and repitched?** Yes, and measure the extra loss in step 2 (PC12).
10. **A neural pitch detector?** **Yes, from the start** (PC2). This replaces the recommended pYIN and adds ONNX
    Runtime and a model file.
11. **The autotune node?** Later, with its own short design (PC13).

New questions from answer 10, each with a recommended answer:

12. **Which model?** *Recommended:* decide in step 1 by measurement. Build FCPE and CREPE behind `PitchModel`, compare
    them on the owner's recordings and on speed, and ship one. FCPE is the expected default (trained on singing, fast).
13. **ONNX Runtime or own inference code?** *Recommended:* ONNX Runtime. Writing the inference for CREPE's convolutions
    by hand would avoid the dependency but be slow without optimised kernels, and would tie DAWg to one model.

## 12. Summary of decisions

| # | Decision | Status |
|---|----------|--------|
| PC1 | Detection is a pure, versioned, cached analysis with no access to the model | Recommended |
| PC2 | A neural pitch model (FCPE or CREPE, chosen in step 1) through ONNX Runtime, then Viterbi smoothing | Owner |
| PC3 | Segmentation extends `pitchNotes`: onset splits, glide split points, lead-ins, centre, vibrato | Recommended |
| PC4 | Each note's slow movement fitted as a `Curve` of smooth (monotone cubic) segments, in frames | Owner |
| PC5 | `AudioFile::pitch`: notes with detected and user centre and shape, pivots, vibrato; per file with "Make copy" | Owner |
| PC6 | Correction Δ = centre move + (user shape − detected shape) + vibrato scaling; detail always kept | Recommended |
| PC7 | Shapes edited with the automation editor's gestures and `curveEditing.ts`; flatten, reset; the centre follows the shape | Owner |
| PC8 | Tilting around pivots moves the nodes on one side along a line; several pivots tilt spans independently | Owner |
| PC9 | Note timing edits write timing markers; musical files only | Owner |
| PC10 | Correction tools as commands: snap to semitone or key with an amount, vibrato (no timing quantise, no straighten) | Recommended |
| PC11 | TD-PSOLA (own code) behind `PitchShifter`, chosen over Rubber Band's real-time mode in the trial | Owner |
| PC12 | Length-preserving phrase renders before the stretch, in the shared cache and queue | Owner (two stages) |
| PC13 | Autotune as a separate real-time effect node, later, own design | Owner |
| PC14 | Polyphonic editing out of scope | Recommended |

## 13. References

* A. de Cheveigné, H. Kawahara: "YIN, a fundamental frequency estimator for speech and music", JASA 111(4), 2002.
* M. Mauch, S. Dixon: "pYIN: A fundamental frequency estimator using probabilistic threshold distributions",
  ICASSP 2014.
* M. Mauch et al.: "Computer-aided melody note transcription using the Tony software: accuracy and efficiency",
  TENOR 2015.
* J. W. Kim, J. Salamon, P. Li, J. P. Bello: "CREPE: A Convolutional Representation for Pitch Estimation",
  ICASSP 2018.
* E. Moulines, F. Charpentier: "Pitch-synchronous waveform processing techniques for text-to-speech synthesis using
  diphones", Speech Communication 9, 1990.
* J. Laroche, M. Dolson: "Improved phase vocoder time-scale modification of audio", IEEE Trans. Speech and Audio
  Processing, 1999.
* A. Röbel, X. Rodet: "Efficient spectral envelope estimation and its application to pitch shifting and envelope
  preservation", DAFx 2005.
* J. Driedger, M. Müller: "A Review of Time-Scale Modification of Music Signals", Applied Sciences, 2016.
* M. Morise, F. Yokomori, K. Ozawa: "WORLD: a vocoder-based high-quality speech synthesis system for real-time
  applications", IEICE Trans. Inf. & Syst., 2016.
* S. Wager et al.: "Deep Autotuner: A pitch correcting network for singing performances", ICASSP 2020.
* Rubber Band Library API documentation (real-time pitch changes, `OptionPitchHighConsistency`):
  https://breakfastquay.com/rubberband/code-doc/

The references were listed from memory; check titles and years before citing them elsewhere.
