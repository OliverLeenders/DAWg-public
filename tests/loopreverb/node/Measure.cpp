#include "Measure.h"

#include "Nodes.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>

namespace measure
{

namespace
{
[[nodiscard]] size_t toIndex (double seconds, double sampleRate, size_t limit)
{
    return std::min (limit, static_cast<size_t> (std::max (0.0, std::round (seconds * sampleRate))));
}

void setParams (dawg::Node& node, const Params& params)
{
    for (const auto& [id, value] : params)
        if (const auto index = node.descriptor().indexOfParam (id); index >= 0)
            node.params().set (static_cast<size_t> (index), value);
}

// One RBJ highpass biquad (Q 0.707), direct form I, in double.
void highpassStage (std::vector<double>& x, double cornerHz, double sampleRate)
{
    const auto w = 2.0 * std::numbers::pi * cornerHz / sampleRate;
    const auto alpha = std::sin (w) / (2.0 * std::sqrt (0.5));
    const auto cw = std::cos (w);
    const auto a0 = 1.0 + alpha;
    const auto b0 = (1.0 + cw) / 2.0 / a0;
    const auto b1 = -(1.0 + cw) / a0;
    const auto a1 = -2.0 * cw / a0;
    const auto a2 = (1.0 - alpha) / a0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    for (auto& sample : x)
    {
        const auto y = b0 * sample + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = sample;
        y2 = y1;
        y1 = y;
        sample = y;
    }
}

// In-place radix-2 FFT (Cooley-Tukey, iterative) [TXT]; the size must be a power of two.
void fft (std::vector<std::complex<double>>& x, bool inverse)
{
    const auto n = x.size();

    for (size_t i = 1, j = 0; i < n; ++i)
    {
        auto bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap (x[i], x[j]);
    }

    for (size_t length = 2; length <= n; length <<= 1)
    {
        const auto angle = 2.0 * std::numbers::pi / static_cast<double> (length) * (inverse ? 1.0 : -1.0);
        const std::complex<double> step (std::cos (angle), std::sin (angle));

        for (size_t start = 0; start < n; start += length)
        {
            std::complex<double> w (1.0, 0.0);
            for (size_t k = 0; k < length / 2; ++k)
            {
                const auto a = x[start + k];
                const auto b = x[start + k + length / 2] * w;
                x[start + k] = a + b;
                x[start + k + length / 2] = a - b;
                w *= step;
            }
        }
    }

    if (inverse)
        for (auto& v : x)
            v /= static_cast<double> (n);
}

// A surrogate of a signal: the same magnitude spectrum with random phases, i.e. stationary Gaussian-like noise with
// the same spectrum and no time variation.
[[nodiscard]] std::vector<float> surrogate (const std::vector<float>& signal, unsigned seed)
{
    size_t n = 1;
    while (n * 2 <= signal.size())
        n *= 2;

    std::vector<std::complex<double>> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = signal[i];

    fft (x, false);
    std::mt19937 random (seed);
    std::uniform_real_distribution<double> phase (0.0, 2.0 * std::numbers::pi);

    for (size_t k = 1; k < n / 2; ++k)
    {
        x[k] = std::polar (std::abs (x[k]), phase (random));
        x[n - k] = std::conj (x[k]);
    }

    fft (x, true);
    std::vector<float> result (n);
    for (size_t i = 0; i < n; ++i)
        result[i] = static_cast<float> (x[i].real());
    return result;
}

// One RBJ lowpass biquad (Q 0.707), direct form I, in double.
void lowpassStage (std::vector<double>& x, double cornerHz, double sampleRate)
{
    const auto w = 2.0 * std::numbers::pi * cornerHz / sampleRate;
    const auto alpha = std::sin (w) / (2.0 * std::sqrt (0.5));
    const auto cw = std::cos (w);
    const auto a0 = 1.0 + alpha;
    const auto b0 = (1.0 - cw) / 2.0 / a0;
    const auto b1 = (1.0 - cw) / a0;
    const auto a1 = -2.0 * cw / a0;
    const auto a2 = (1.0 - alpha) / a0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    for (auto& sample : x)
    {
        const auto y = b0 * sample + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = sample;
        y2 = y1;
        y1 = y;
        sample = y;
    }
}

[[nodiscard]] double percentile (std::vector<double> values, double fraction)
{
    if (values.empty())
        return 0.0;

    std::sort (values.begin(), values.end());
    const auto index = static_cast<size_t> (fraction * static_cast<double> (values.size() - 1) + 0.5);
    return values[index];
}

// One RBJ bandpass biquad (constant 0 dB peak gain), direct form I, in double.
void bandpass (std::vector<double>& x, double centreHz, double sampleRate)
{
    const auto w = 2.0 * std::numbers::pi * centreHz / sampleRate;
    const auto q = std::sqrt (2.0); // one octave
    const auto alpha = std::sin (w) / (2.0 * q);
    const auto a0 = 1.0 + alpha;
    const auto b0 = alpha / a0;
    const auto b2 = -alpha / a0;
    const auto a1 = -2.0 * std::cos (w) / a0;
    const auto a2 = (1.0 - alpha) / a0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    for (auto& sample : x)
    {
        const auto y = b0 * sample + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = sample;
        y2 = y1;
        y1 = y;
        sample = y;
    }
}
} // namespace

harness::Audio impulseResponse (const Params& params, double seconds, double sampleRate)
{
    auto node = nodes::make ("dawg.loopReverb");
    harness::RenderOptions options;
    options.sampleRate = sampleRate;
    options.params = params;
    const auto input = harness::impulse (seconds, sampleRate);
    return harness::render (*node, input, options);
}

harness::Audio renderChanging (const harness::Audio& input, const Params& params, int blockSize,
                               const std::function<void (dawg::Node&, size_t)>& change)
{
    auto node = nodes::make ("dawg.loopReverb");
    setParams (*node, params);
    node->prepare ({ input.sampleRate, blockSize });

    harness::Audio out;
    out.sampleRate = input.sampleRate;
    out.left.assign (input.left.size(), 0.0f);
    out.right.assign (input.right.size(), 0.0f);
    dawg::Transport transport;

    for (size_t position = 0; position < input.left.size(); position += static_cast<size_t> (blockSize))
    {
        change (*node, position);
        const auto size = std::min (static_cast<size_t> (blockSize), input.left.size() - position);
        const std::array<dawg::AudioInput, 1> inputs { { { input.left.data() + position,
                                                           input.right.data() + position } } };
        const std::array<dawg::AudioOutput, 1> outputs { { { out.left.data() + position,
                                                             out.right.data() + position } } };
        dawg::ProcessContext context;
        context.numSamples = static_cast<int> (size);
        context.audioInputs = inputs;
        context.audioOutputs = outputs;
        context.transport = &transport;
        node->process (context);
    }

    return out;
}

std::vector<float> octaveBand (const std::vector<float>& signal, double centreHz, double sampleRate)
{
    std::vector<double> x (signal.begin(), signal.end());
    bandpass (x, centreHz, sampleRate);
    bandpass (x, centreHz, sampleRate);

    std::vector<float> result (x.size());
    std::transform (x.begin(), x.end(), result.begin(), [] (double sample) { return static_cast<float> (sample); });
    return result;
}

double reverberationTime (const std::vector<double>& energy, double sampleRate)
{
    // Schroeder's backward integration.
    std::vector<double> decay (energy.size());
    double sum = 0.0;

    for (size_t i = energy.size(); i-- > 0;)
    {
        sum += energy[i];
        decay[i] = sum;
    }

    if (sum <= 0.0)
        return 0.0;

    // Least squares through the curve between -5 and -25 dB.
    double n = 0.0, st = 0.0, sd = 0.0, stt = 0.0, std2 = 0.0;
    bool reached = false;

    for (size_t i = 0; i < decay.size(); ++i)
    {
        const auto db = 10.0 * std::log10 (decay[i] / sum + 1.0e-300);

        if (db > -5.0)
            continue;

        if (db < -25.0)
        {
            reached = true;
            break;
        }

        const auto t = static_cast<double> (i) / sampleRate;
        n += 1.0;
        st += t;
        sd += db;
        stt += t * t;
        std2 += t * db;
    }

    if (! reached || n < 2.0)
        return 0.0;

    const auto slope = (n * std2 - st * sd) / (n * stt - st * st);
    return slope < 0.0 ? -60.0 / slope : 0.0;
}

std::vector<double> rt60Bands (const harness::Audio& response)
{
    std::vector<double> result;

    for (const auto centre : { 500.0, 1000.0, 2000.0 })
    {
        const auto left = octaveBand (response.left, centre, response.sampleRate);
        const auto right = octaveBand (response.right, centre, response.sampleRate);
        std::vector<double> energy (left.size());

        for (size_t i = 0; i < energy.size(); ++i)
            energy[i] = static_cast<double> (left[i]) * left[i] + static_cast<double> (right[i]) * right[i];

        result.push_back (reverberationTime (energy, response.sampleRate));
    }

    return result;
}

double rt60 (const harness::Audio& response)
{
    const auto bands = rt60Bands (response);
    double sum = 0.0;
    for (const auto t : bands)
        sum += t;
    return sum / static_cast<double> (bands.size());
}

double rt60At (const harness::Audio& response, double centreHz)
{
    const auto left = octaveBand (response.left, centreHz, response.sampleRate);
    const auto right = octaveBand (response.right, centreHz, response.sampleRate);
    std::vector<double> energy (left.size());

    for (size_t i = 0; i < energy.size(); ++i)
        energy[i] = static_cast<double> (left[i]) * left[i] + static_cast<double> (right[i]) * right[i];

    return reverberationTime (energy, response.sampleRate);
}

double bandEnergyDb (const harness::Audio& signal, double lowHz, double highHz, double fromSecs, double toSecs)
{
    const auto from = toIndex (fromSecs, signal.sampleRate, signal.left.size());
    const auto to = toIndex (toSecs, signal.sampleRate, signal.left.size());
    double energy = 0.0;

    for (const auto* channel : { &signal.left, &signal.right })
    {
        std::vector<double> x (channel->begin(), channel->end());
        highpassStage (x, lowHz, signal.sampleRate);
        highpassStage (x, lowHz, signal.sampleRate);

        if (highHz > 0.0)
        {
            lowpassStage (x, highHz, signal.sampleRate);
            lowpassStage (x, highHz, signal.sampleRate);
        }

        for (auto i = from; i < to; ++i)
            energy += x[i] * x[i];
    }

    return 10.0 * std::log10 (energy + 1.0e-30);
}

double correlation (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    to = std::min ({ to, a.size(), b.size() });

    for (auto i = from; i < to; ++i)
    {
        ab += static_cast<double> (a[i]) * b[i];
        aa += static_cast<double> (a[i]) * a[i];
        bb += static_cast<double> (b[i]) * b[i];
    }

    return aa > 0.0 && bb > 0.0 ? ab / std::sqrt (aa * bb) : 0.0;
}

double correlation (const harness::Audio& a, const harness::Audio& b, double fromSecs, double toSecs)
{
    const auto from = toIndex (fromSecs, a.sampleRate, a.left.size());
    const auto to = toIndex (toSecs, a.sampleRate, a.left.size());
    std::vector<float> x (a.left.begin() + static_cast<long> (from), a.left.begin() + static_cast<long> (to));
    std::vector<float> y (b.left.begin() + static_cast<long> (from), b.left.begin() + static_cast<long> (to));
    x.insert (x.end(), a.right.begin() + static_cast<long> (from), a.right.begin() + static_cast<long> (to));
    y.insert (y.end(), b.right.begin() + static_cast<long> (from), b.right.begin() + static_cast<long> (to));
    return correlation (x, y, 0, x.size());
}

double largestCrossCorrelation (const harness::Audio& signal, size_t from, size_t to, double maxLagSecs)
{
    const auto& l = signal.left;
    const auto& r = signal.right;
    to = std::min (to, l.size());
    const auto maxLag = static_cast<long> (std::round (maxLagSecs * signal.sampleRate));
    double ll = 0.0, rr = 0.0;

    for (auto i = from; i < to; ++i)
    {
        ll += static_cast<double> (l[i]) * l[i];
        rr += static_cast<double> (r[i]) * r[i];
    }

    if (ll <= 0.0 || rr <= 0.0)
        return 0.0;

    double best = -1.0;

    for (auto lag = -maxLag; lag <= maxLag; ++lag)
    {
        double lr = 0.0;
        for (auto i = static_cast<long> (from); i < static_cast<long> (to); ++i)
        {
            const auto j = i + lag;
            if (j >= static_cast<long> (from) && j < static_cast<long> (to))
                lr += static_cast<double> (l[static_cast<size_t> (i)]) * r[static_cast<size_t> (j)];
        }
        best = std::max (best, lr / std::sqrt (ll * rr));
    }

    return best;
}

double envelopeCorrelation (const harness::Audio& signal, size_t from, size_t to, double smoothSecs)
{
    to = std::min (to, signal.left.size());
    const auto window = std::max<size_t> (1, static_cast<size_t> (std::round (smoothSecs * signal.sampleRate)));

    const auto envelope = [from, to, window] (const std::vector<float>& x)
    {
        std::vector<double> e (to - from, 0.0);
        double sum = 0.0;

        for (auto i = from; i < to; ++i)
        {
            sum += static_cast<double> (x[i]) * x[i];
            if (i >= from + window)
                sum -= static_cast<double> (x[i - window]) * x[i - window];
            e[i - from] = sum / static_cast<double> (window);
        }

        return e;
    };

    const auto l = envelope (signal.left);
    const auto r = envelope (signal.right);
    double ml = 0.0, mr = 0.0;
    for (size_t i = 0; i < l.size(); ++i)
    {
        ml += l[i] / static_cast<double> (l.size());
        mr += r[i] / static_cast<double> (r.size());
    }

    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (size_t i = 0; i < l.size(); ++i)
    {
        lr += (l[i] - ml) * (r[i] - mr);
        ll += (l[i] - ml) * (l[i] - ml);
        rr += (r[i] - mr) * (r[i] - mr);
    }

    return ll > 0.0 && rr > 0.0 ? lr / std::sqrt (ll * rr) : 0.0;
}

double channelEnergyDb (const harness::Audio& signal, size_t channel, double fromSecs, double toSecs)
{
    const auto& samples = channel == 0 ? signal.left : signal.right;
    const auto from = toIndex (fromSecs, signal.sampleRate, samples.size());
    const auto to = toIndex (toSecs, signal.sampleRate, samples.size());
    double energy = 0.0;

    for (auto i = from; i < to; ++i)
        energy += static_cast<double> (samples[i]) * samples[i];

    return 10.0 * std::log10 (energy + 1.0e-30);
}

double echoDensity (const harness::Audio& response, double fromSecs, double toSecs)
{
    float peak = 0.0f;
    for (const auto* channel : { &response.left, &response.right })
        for (const auto sample : *channel)
            peak = std::max (peak, std::abs (sample));

    const auto threshold = 0.01f * peak; // -40 dB
    const auto from = std::max<size_t> (1, toIndex (fromSecs, response.sampleRate, response.left.size()));
    const auto to = toIndex (toSecs, response.sampleRate, response.left.size() - 1);
    double count = 0.0;

    for (const auto* channel : { &response.left, &response.right })
    {
        const auto& h = *channel;
        for (auto i = from; i < to; ++i)
        {
            const auto m = std::abs (h[i]);
            if (m > threshold && m > std::abs (h[i - 1]) && m >= std::abs (h[i + 1]))
                count += 1.0;
        }
    }

    const auto windows = (toSecs - fromSecs) / 0.01;
    return count / 2.0 / windows;
}

double bandLevelDb (const harness::Audio& signal, double centreHz, double fromSecs, double toSecs)
{
    const auto left = octaveBand (signal.left, centreHz, signal.sampleRate);
    const auto right = octaveBand (signal.right, centreHz, signal.sampleRate);
    const auto from = toIndex (fromSecs, signal.sampleRate, left.size());
    const auto to = toIndex (toSecs, signal.sampleRate, left.size());
    double energy = 0.0;

    for (auto i = from; i < to; ++i)
        energy += static_cast<double> (left[i]) * left[i] + static_cast<double> (right[i]) * right[i];

    return 10.0 * std::log10 (energy + 1.0e-30);
}

double rmsDb (const harness::Audio& signal, double fromSecs, double toSecs)
{
    const auto from = toIndex (fromSecs, signal.sampleRate, signal.left.size());
    const auto to = toIndex (toSecs, signal.sampleRate, signal.left.size());
    double energy = 0.0;

    for (auto i = from; i < to; ++i)
        energy += static_cast<double> (signal.left[i]) * signal.left[i]
                  + static_cast<double> (signal.right[i]) * signal.right[i];

    return 10.0 * std::log10 (energy / (2.0 * static_cast<double> (std::max<size_t> (1, to - from))) + 1.0e-30);
}

harness::Audio pinkNoise (double seconds, double sampleRate)
{
    auto audio = harness::silence (seconds, sampleRate);
    std::mt19937 random (1979);
    std::uniform_real_distribution<float> uniform (-1.0f, 1.0f);
    constexpr size_t rows = 16;

    for (auto* channel : { &audio.left, &audio.right })
    {
        std::array<float, rows> value {};
        for (auto& v : value)
            v = uniform (random);

        float sum = 0.0f;
        for (const auto v : value)
            sum += v;

        double energy = 0.0;

        for (size_t i = 0; i < channel->size(); ++i)
        {
            // Row r changes when bit r of the counter flips from 0 to 1: every 2^r samples.
            const auto counter = i + 1;
            const auto row = static_cast<size_t> (std::countr_zero (counter));

            if (row < rows)
            {
                sum -= value[row];
                value[row] = uniform (random);
                sum += value[row];
            }

            (*channel)[i] = sum + uniform (random);
            energy += static_cast<double> ((*channel)[i]) * (*channel)[i];
        }

        const auto scale = 0.25 / std::sqrt (energy / static_cast<double> (channel->size()));
        for (auto& sample : *channel)
            sample = static_cast<float> (sample * scale);
    }

    return audio;
}

harness::Audio sine (double hz, double amplitude, double seconds, double sampleRate)
{
    auto audio = harness::silence (seconds, sampleRate);

    for (size_t i = 0; i < audio.left.size(); ++i)
        audio.left[i] = audio.right[i] = static_cast<float> (
            amplitude * std::sin (2.0 * std::numbers::pi * hz * static_cast<double> (i) / sampleRate));

    return audio;
}

std::vector<float> highpass (const std::vector<float>& signal, double cornerHz, double sampleRate)
{
    std::vector<double> x (signal.begin(), signal.end());
    highpassStage (x, cornerHz, sampleRate);
    highpassStage (x, cornerHz, sampleRate);

    std::vector<float> result (x.size());
    std::transform (x.begin(), x.end(), result.begin(), [] (double sample) { return static_cast<float> (sample); });
    return result;
}

std::vector<double> densityProfile (const harness::Audio& response, double fromSecs, double toSecs)
{
    constexpr double gaussianOutside = 0.31731;
    const auto window = static_cast<size_t> (std::round (0.005 * response.sampleRate));
    const auto from = toIndex (fromSecs, response.sampleRate, response.left.size());
    const auto to = toIndex (toSecs, response.sampleRate, response.left.size());
    const auto left = highpass (response.left, 4000.0, response.sampleRate);
    const auto right = highpass (response.right, 4000.0, response.sampleRate);
    std::vector<double> profile;

    for (auto start = from; start + window <= to; start += window)
    {
        double sum = 0.0;

        for (const auto* band : { &left, &right })
        {
            double energy = 0.0;
            for (auto i = start; i < start + window; ++i)
                energy += static_cast<double> ((*band)[i]) * (*band)[i];

            const auto sigma = std::sqrt (energy / static_cast<double> (window));
            double outside = 0.0;
            for (auto i = start; i < start + window; ++i)
                outside += std::abs ((*band)[i]) > sigma ? 1.0 : 0.0;

            sum += sigma > 0.0 ? outside / static_cast<double> (window) / gaussianOutside : 0.0;
        }

        profile.push_back (sum / 2.0);
    }

    return profile;
}

Smoothness smoothness (const harness::Audio& signal, double fromSecs)
{
    constexpr double cornerHz = 4000.0;
    constexpr double gaussianOutside = 0.31731; // P(|x| > sigma) for Gaussian noise
    const auto window = static_cast<size_t> (std::round (0.01 * signal.sampleRate));
    const auto from = toIndex (fromSecs, signal.sampleRate, signal.left.size());
    std::vector<double> kurtosis, density;

    for (const auto* channel : { &signal.left, &signal.right })
    {
        const auto band = highpass (*channel, cornerHz, signal.sampleRate);
        std::vector<double> energies;

        for (auto start = from; start + window <= band.size(); start += window)
        {
            double sum = 0.0;
            for (auto i = start; i < start + window; ++i)
                sum += static_cast<double> (band[i]) * band[i];
            energies.push_back (sum);
        }

        const auto loudest = energies.empty() ? 0.0 : *std::max_element (energies.begin(), energies.end());

        for (size_t w = 0; w < energies.size(); ++w)
        {
            if (energies[w] < loudest * 1.0e-4 || energies[w] <= 0.0) // 40 dB down: the tail is over
                break;

            const auto start = from + w * window;
            const auto m2 = energies[w] / static_cast<double> (window);
            const auto sigma = std::sqrt (m2);
            double m4 = 0.0, outside = 0.0;

            for (auto i = start; i < start + window; ++i)
            {
                const double x = band[i];
                m4 += x * x * x * x;
                outside += std::abs (x) > sigma ? 1.0 : 0.0;
            }

            m4 /= static_cast<double> (window);
            kurtosis.push_back (m4 / (m2 * m2) - 3.0);
            density.push_back (outside / static_cast<double> (window) / gaussianOutside);
        }
    }

    Smoothness result;
    result.medianExcessKurtosis = percentile (kurtosis, 0.5);
    result.worstExcessKurtosis = percentile (kurtosis, 0.9);
    result.medianDensity = percentile (density, 0.5);
    result.worstDensity = percentile (density, 0.1);
    result.windows = static_cast<int> (kurtosis.size());
    return result;
}

double highFluctuation (const harness::Audio& output, double fromSecs, double toSecs)
{
    const auto from = toIndex (fromSecs, output.sampleRate, output.left.size());
    const auto to = toIndex (toSecs, output.sampleRate, output.left.size());
    const auto frame = static_cast<size_t> (std::round (0.002 * output.sampleRate));

    const auto variation = [frame, &output] (const std::vector<float>& segment)
    {
        const auto band = highpass (segment, 4000.0, output.sampleRate);
        std::vector<double> energies;

        // Skip the first 10 ms, where the highpass settles.
        for (auto start = frame * 5; start + frame <= band.size(); start += frame)
        {
            double sum = 0.0;
            for (auto i = start; i < start + frame; ++i)
                sum += static_cast<double> (band[i]) * band[i];
            energies.push_back (sum);
        }

        double mean = 0.0, square = 0.0;
        for (const auto e : energies)
        {
            mean += e;
            square += e * e;
        }

        const auto n = static_cast<double> (std::max<size_t> (1, energies.size()));
        mean /= n;
        return std::sqrt (std::max (0.0, square / n - mean * mean)) / (mean + 1.0e-30);
    };

    double ratio = 0.0;
    unsigned seed = 1;

    for (const auto* channel : { &output.left, &output.right })
    {
        const std::vector<float> segment (channel->begin() + static_cast<long> (from),
                                          channel->begin() + static_cast<long> (to));
        const auto same = surrogate (segment, seed++);
        const std::vector<float> trimmed (segment.begin(), segment.begin() + static_cast<long> (same.size()));
        ratio += variation (trimmed) / variation (same) / 2.0;
    }

    return ratio;
}

harness::Audio snare (double seconds, double sampleRate)
{
    auto audio = harness::silence (seconds, sampleRate);
    std::mt19937 random (1962);
    std::normal_distribution<float> gaussian (0.0f, 1.0f);
    const auto length = std::min (audio.left.size(), static_cast<size_t> (0.3 * sampleRate));

    for (size_t i = 0; i < length; ++i)
    {
        const auto t = static_cast<double> (i) / sampleRate;
        const auto attack = std::min (1.0, t / 0.003);
        const auto noise = 0.35 * gaussian (random) * std::exp (-t / 0.04);
        const auto tone = 0.4 * std::sin (2.0 * std::numbers::pi * 180.0 * t) * std::exp (-t / 0.025);
        audio.left[i] = audio.right[i] = static_cast<float> (attack * (noise + tone));
    }

    return audio;
}

float largestStep (const harness::Audio& signal, size_t from)
{
    float result = 0.0f;
    for (const auto* channel : { &signal.left, &signal.right })
        for (auto i = std::max<size_t> (from, 1); i < channel->size(); ++i)
            result = std::max (result, std::abs ((*channel)[i] - (*channel)[i - 1]));
    return result;
}

size_t firstAbove (const harness::Audio& signal, float fraction)
{
    float peak = 0.0f;
    for (const auto* channel : { &signal.left, &signal.right })
        for (const auto sample : *channel)
            peak = std::max (peak, std::abs (sample));

    for (size_t i = 0; i < signal.left.size(); ++i)
        if (std::abs (signal.left[i]) >= fraction * peak || std::abs (signal.right[i]) >= fraction * peak)
            return i;

    return signal.left.size();
}

namespace
{
// One biquad, direct form I, in double, forwards over the signal: b0, b1, b2, a1, a2.
void biquad (std::vector<double>& x, const std::array<double, 5>& c)
{
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    for (auto& sample : x)
    {
        const auto y = c[0] * sample + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;
        x2 = x1;
        x1 = sample;
        y2 = y1;
        y1 = y;
        sample = y;
    }
}

// A second-order highpass or lowpass section of quality q (the RBJ form: the bilinear transform with prewarping).
[[nodiscard]] std::array<double, 5> section (double cornerHz, double q, bool high, double sampleRate)
{
    const auto w = 2.0 * std::numbers::pi * cornerHz / sampleRate;
    const auto alpha = std::sin (w) / (2.0 * q);
    const auto cw = std::cos (w);
    const auto a0 = 1.0 + alpha;
    const auto b0 = (high ? (1.0 + cw) : (1.0 - cw)) / 2.0 / a0;
    return { b0, (high ? -2.0 : 2.0) * b0, b0, -2.0 * cw / a0, (1.0 - alpha) / a0 };
}

[[nodiscard]] size_t powerOfTwoAtLeast (size_t n)
{
    size_t m = 1;
    while (m < n)
        m *= 2;
    return m;
}

// A moving average over a centred window of samples (cut at the ends).
[[nodiscard]] std::vector<double> movingAverage (const std::vector<double>& x, size_t window)
{
    std::vector<double> prefix (x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); ++i)
        prefix[i + 1] = prefix[i] + x[i];

    std::vector<double> result (x.size());
    const auto half = window / 2;

    for (size_t i = 0; i < x.size(); ++i)
    {
        const auto from = i >= half ? i - half : 0;
        const auto to = std::min (x.size(), i + half + 1);
        result[i] = (prefix[to] - prefix[from]) / static_cast<double> (to - from);
    }

    return result;
}

[[nodiscard]] std::vector<double> movingMeanSquare (const std::vector<double>& x, size_t window)
{
    std::vector<double> squares (x.size());
    std::transform (x.begin(), x.end(), squares.begin(), [] (double v) { return v * v; });
    return movingAverage (squares, window);
}

[[nodiscard]] double standardDeviation (const std::vector<double>& x)
{
    double mean = 0.0;
    for (const auto v : x)
        mean += v / static_cast<double> (x.size());

    double variance = 0.0;
    for (const auto v : x)
        variance += (v - mean) * (v - mean) / static_cast<double> (x.size());

    return std::sqrt (variance);
}

// A14's roughness of one segment: its 2 ms frame energies in dB minus their 25-frame centred moving average.
[[nodiscard]] double roughnessOf (const std::vector<double>& x, double sampleRate)
{
    const auto frame = static_cast<size_t> (std::round (0.002 * sampleRate));
    std::vector<double> db;

    for (size_t start = 0; start + frame <= x.size(); start += frame)
    {
        double e = 0.0;
        for (auto i = start; i < start + frame; ++i)
            e += x[i] * x[i];
        db.push_back (10.0 * std::log10 (e / static_cast<double> (frame) + 1.0e-300));
    }

    constexpr size_t half = 12; // 25 frames: 50 ms
    std::vector<double> residual;

    for (auto i = half; i + half < db.size(); ++i)
    {
        double mean = 0.0;
        for (auto j = i - half; j <= i + half; ++j)
            mean += db[j] / (2.0 * half + 1.0);
        residual.push_back (db[i] - mean);
    }

    return residual.size() > 1 ? standardDeviation (residual) : 0.0;
}
} // namespace

std::vector<double> butterworth (const std::vector<double>& signal, double cornerHz, bool high, double sampleRate)
{
    auto x = signal;

    for (int pass = 0; pass < 2; ++pass)
    {
        for (const auto q : { 0.54119610, 1.30656296 })
            biquad (x, section (cornerHz, q, high, sampleRate));
        std::reverse (x.begin(), x.end());
    }

    return x;
}

Roughness highRoughness (const harness::Audio& response)
{
    const auto rate = response.sampleRate;
    const auto from = static_cast<long> (toIndex (0.2, rate, response.left.size()));
    const auto to = static_cast<long> (toIndex (1.0, rate, response.left.size()));
    const auto window = static_cast<size_t> (std::round (0.05 * rate));
    Roughness result;
    unsigned draw = 1;

    for (const auto* channel : { &response.left, &response.right })
    {
        const auto band = butterworth (std::vector<double> (channel->begin(), channel->end()), 8000.0, true, rate);
        const std::vector<double> segment (band.begin() + from, band.begin() + to);
        result.response += roughnessOf (segment, rate) / 2.0;
        const auto target = movingMeanSquare (segment, window);

        for (int k = 0; k < 4; ++k)
        {
            // The same magnitude spectrum (of the zero-padded segment), uniformly random phases.
            const auto n = powerOfTwoAtLeast (segment.size());
            std::vector<std::complex<double>> x (n);
            for (size_t i = 0; i < segment.size(); ++i)
                x[i] = segment[i];

            fft (x, false);
            std::mt19937 random (draw++);
            std::uniform_real_distribution<double> phase (0.0, 2.0 * std::numbers::pi);
            for (size_t i = 1; i < n / 2; ++i)
            {
                x[i] = std::polar (std::abs (x[i]), phase (random));
                x[n - i] = std::conj (x[i]);
            }
            fft (x, true);

            std::vector<double> copy (segment.size());
            for (size_t i = 0; i < copy.size(); ++i)
                copy[i] = x[i].real();

            const auto actual = movingMeanSquare (copy, window);
            for (size_t i = 0; i < copy.size(); ++i)
                copy[i] *= actual[i] > 0.0 ? std::sqrt (target[i] / actual[i]) : 0.0;

            result.reference += roughnessOf (copy, rate) / 8.0;
        }
    }

    return result;
}

harness::Audio heldNote (double hz, double sampleRate)
{
    harness::Audio note;
    note.sampleRate = sampleRate;
    const auto end = static_cast<size_t> (std::round (8.0 * sampleRate));
    const auto fade = static_cast<size_t> (std::round (0.005 * sampleRate));
    note.left.assign (static_cast<size_t> (std::round (18.0 * sampleRate)), 0.0f);

    for (size_t i = 0; i < end; ++i)
    {
        const auto t = static_cast<double> (i) / sampleRate;
        const auto gain = i + fade < end ? 1.0 : static_cast<double> (end - i) / static_cast<double> (fade);
        note.left[i] = static_cast<float> (0.25 * gain * std::sin (2.0 * std::numbers::pi * hz * t));
    }

    note.right = note.left;
    return note;
}

harness::Audio idealReverb (const harness::Audio& input, double t60, unsigned seed)
{
    const auto rate = input.sampleRate;
    const auto length = static_cast<size_t> (std::round (std::min (3.0 * t60, 18.0) * rate));
    auto used = input.left.size();
    while (used > 0 && input.left[used - 1] == 0.0f)
        --used;

    // Two real responses in one complex FFT: the input is real, so the real and imaginary parts of the product's
    // inverse are its convolutions with the two responses.
    const auto n = powerOfTwoAtLeast (used + length);
    std::vector<std::complex<double>> x (n), h (n);
    for (size_t i = 0; i < used; ++i)
        x[i] = input.left[i];

    std::mt19937 random (seed);
    std::normal_distribution<double> gauss (0.0, 1.0);
    for (size_t i = 0; i < length; ++i)
    {
        const auto decay = std::pow (10.0, -3.0 * static_cast<double> (i) / rate / t60);
        const auto l = gauss (random);
        const auto r = gauss (random);
        h[i] = { l * decay, r * decay };
    }

    fft (x, false);
    fft (h, false);
    for (size_t i = 0; i < n; ++i)
        x[i] *= h[i];
    fft (x, true);

    harness::Audio out;
    out.sampleRate = rate;
    out.left.assign (input.left.size(), 0.0f);
    out.right.assign (input.left.size(), 0.0f);
    for (size_t i = 0; i < std::min (n, out.left.size()); ++i)
    {
        out.left[i] = static_cast<float> (x[i].real());
        out.right[i] = static_cast<float> (x[i].imag());
    }

    return out;
}

NoteMotion noteMotion (const std::vector<float>& channel, double hz, double sampleRate)
{
    const auto from = toIndex (5.0, sampleRate, channel.size());
    const auto to = toIndex (7.9, sampleRate, channel.size());
    // The filters run forwards and backwards: band-pass up to a little after the window.
    const auto end = static_cast<long> (toIndex (8.5, sampleRate, channel.size()));

    auto x = butterworth (std::vector<double> (channel.begin(), channel.begin() + end), 0.8 * hz, true, sampleRate);
    x = butterworth (x, 1.25 * hz, false, sampleRate);

    // The analytic signal: positive frequencies doubled, negative ones removed [TXT].
    const auto n = powerOfTwoAtLeast (x.size());
    std::vector<std::complex<double>> z (n);
    for (size_t i = 0; i < x.size(); ++i)
        z[i] = x[i];

    fft (z, false);
    for (size_t i = 1; i < n / 2; ++i)
        z[i] *= 2.0;
    for (auto i = n / 2 + 1; i < n; ++i)
        z[i] = 0.0;
    fft (z, true);

    std::vector<double> db (to - from), cents (to - from);
    for (auto i = from; i < to; ++i)
    {
        db[i - from] = 20.0 * std::log10 (std::abs (z[i]) + 1.0e-300);
        const auto frequency = std::arg (z[i] * std::conj (z[i - 1])) * sampleRate / (2.0 * std::numbers::pi);
        cents[i - from] = 1200.0 * std::log2 (std::max (frequency, 1.0e-3) / hz);
    }

    const auto window = static_cast<size_t> (std::round (0.02 * sampleRate));
    const auto smoothDb = movingAverage (db, window);
    NoteMotion result;
    result.swingDb =
        *std::max_element (smoothDb.begin(), smoothDb.end()) - *std::min_element (smoothDb.begin(), smoothDb.end());
    result.driftCents = standardDeviation (movingAverage (cents, window));
    return result;
}

double releaseUnevenness (const std::vector<float>& channel, double sampleRate)
{
    const auto frame = static_cast<size_t> (std::round (0.1 * sampleRate));
    std::vector<double> db;

    for (auto start = toIndex (8.05, sampleRate, channel.size()); start + frame <= channel.size(); start += frame)
    {
        double e = 0.0;
        for (auto i = start; i < start + frame; ++i)
            e += static_cast<double> (channel[i]) * channel[i];
        db.push_back (10.0 * std::log10 (e / static_cast<double> (frame) + 1.0e-300));
        if (db.back() <= db.front() - 30.0)
            break;
    }

    const auto count = db.size();
    if (count < 5)
        return 0.0;

    // Least-squares cubic over time (normalised to [-1, 1] for conditioning), by the normal equations and Gaussian
    // elimination.
    const auto time = [count] (size_t i)
    { return 2.0 * static_cast<double> (i) / static_cast<double> (count - 1) - 1.0; };
    std::array<std::array<double, 5>, 4> m {};

    for (size_t i = 0; i < count; ++i)
    {
        const auto t = time (i);
        const std::array<double, 4> p { 1.0, t, t * t, t * t * t };
        for (size_t r = 0; r < 4; ++r)
        {
            for (size_t c = 0; c < 4; ++c)
                m[r][c] += p[r] * p[c];
            m[r][4] += p[r] * db[i];
        }
    }

    for (size_t col = 0; col < 4; ++col)
        for (auto r = col + 1; r < 4; ++r)
        {
            const auto f = m[r][col] / m[col][col];
            for (auto c = col; c < 5; ++c)
                m[r][c] -= f * m[col][c];
        }

    std::array<double, 4> a {};
    for (size_t r = 4; r-- > 0;)
    {
        auto v = m[r][4];
        for (auto c = r + 1; c < 4; ++c)
            v -= m[r][c] * a[c];
        a[r] = v / m[r][r];
    }

    double squares = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        const auto t = time (i);
        const auto d = db[i] - (a[0] + a[1] * t + a[2] * t * t + a[3] * t * t * t);
        squares += d * d;
    }

    return std::sqrt (squares / static_cast<double> (count));
}

} // namespace measure
