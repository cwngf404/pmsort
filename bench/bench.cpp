
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "pmsort.hpp"

using namespace std;
using Clock = chrono::steady_clock;
using ll = long long;

// config

constexpr size_t N = 10'000'000;
constexpr int RUNS = 10;
constexpr size_t NEARLY_SWAPS = N / 1000;

constexpr uint32_t SEED = 4042026;

constexpr int DUP_RANGE = 10'000;
constexpr int ZIPF_RANGE = 100'000;
constexpr double ZIPF_EXPONENT = 1.2;

enum Algorithm {
    PMS = 0,
    STD_SORT = 1,
    STABLE_SORT = 2
};

const array<string, 3> ALG_NAMES = {
    "PMS",
    "std::sort",
    "std::stable_sort"
};

const array<string, 6> CASE_NAMES = {
    "Random",
    "Duplicates",
    "Nearly sorted",
    "Pipe organ",
    "Reverse",
    "Zipf"
};

mt19937 rng(SEED);
mt19937_64 rng64(SEED);

// Generate one Zipf distributed 
vector<double> buildZipfCDF() {
    vector<double> cdf(ZIPF_RANGE);
    double total = 0.0;

    for (int i = 1; i <= ZIPF_RANGE; ++i) {
        total += 1.0 / pow(static_cast<double>(i),
                           ZIPF_EXPONENT);
        cdf[i - 1] = total;
    }

    for (double& x : cdf)
        x /= total;

    return cdf;
}

int sampleZipf(const vector<double>& cdf,
               uniform_real_distribution<double>& dist) {
    double x = dist(rng64);

    auto it = lower_bound(cdf.begin(), cdf.end(), x);

    return static_cast<int>(it - cdf.begin()) + 1;
}

// Data generator

vector<int> generateData(int type,
                         const vector<double>& zipfCDF) {
    vector<int> a(N);

    switch (type) {
    case 0: { // Random
        uniform_int_distribution<int> dist(1, 1'000'000'000);

        for (int& x : a)
            x = dist(rng);

        break;
    }

    case 1: { // Duplicates:1- 10,000
        uniform_int_distribution<int> dist(1, DUP_RANGE);

        for (int& x : a)
            x = dist(rng);

        break;
    }

    case 2: { // Nearly sorted
        iota(a.begin(), a.end(), 0);

        uniform_int_distribution<size_t> pos(0, N - 1);

        for (size_t i = 0; i < NEARLY_SWAPS; ++i)
            swap(a[pos(rng)], a[pos(rng)]);

        break;
    }

    case 3: { // Pipe organ: 0, 1, 2, ..., peak, ... , 2, 1, 0
        for (size_t i = 0; i < N; ++i) {
            a[i] = static_cast<int>(
                min(i, N - 1 - i)
            );
        }

        break;
    }

    case 4: { // Reverse sorted
        iota(a.begin(), a.end(), 0);
        reverse(a.begin(), a.end());
        break;
    }

    case 5: { // Zipf distribution
        uniform_real_distribution<double> dist(0.0, 1.0);

        for (int& x : a)
            x = sampleZipf(zipfCDF, dist);

        break;
    }
    }

    return a;
}

// Algorithm

void runSort(int algorithm, vector<int>& a) {
    switch (algorithm) {
    case PMS:
        pms(a.begin(), a.end(), less<int>{});
        break;

    case STD_SORT:
        sort(a.begin(), a.end());
        break;

    case STABLE_SORT:
        stable_sort(a.begin(), a.end());
        break;
    }
}

bool isSorted(const vector<int>& a) {
    return is_sorted(a.begin(), a.end());
}

// Bench

double benchmarkOne(int algorithm,
                    const vector<int>& original,
                    vector<int>& work,
                    bool& valid) {
    // Exclude copy time from sorting time.
    work = original;

    const auto start = Clock::now();

    runSort(algorithm, work);

    const auto end = Clock::now();

    valid = isSorted(work);

    return chrono::duration<double, milli>(
        end - start
    ).count();
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    cout << fixed << setprecision(3);

   cout << "Benchmark\n";

    cout << "Building Zipf CDF...\n";
    const vector<double> zipfCDF = buildZipfCDF();

    cout << "Running warmup...\n";

    vector<int> warmup = generateData(0, zipfCDF);
    vector<int> work;

    for (int algorithm = 0; algorithm < 3; ++algorithm) {
        bool valid = false;

        double ms = benchmarkOne(
            algorithm, warmup, work, valid
        );

        cout << "Warmup " << ALG_NAMES[algorithm]
             << ": " << ms << " ms"
             << " | Sorted: " << (valid ? "yes" : "no")
             << '\n';

        if (!valid) {
            cerr << "errkr: Warmup failed.\n";
            return 1;
        }
    }

    warmup.clear();
    warmup.shrink_to_fit();
    work.clear();
    work.shrink_to_fit();
    cout << "\nRunning...\n";

    // times[case][algorithm][run]
    array<array<array<double, RUNS>, 3>, 6> times{};

    for (int testCase = 0; testCase < 6; ++testCase) {
        cout << "Case: " << CASE_NAMES[testCase] << '\n';

        const vector<int> original =
            generateData(testCase, zipfCDF);

        for (int run = 0; run < RUNS; ++run) {
            cout << "Run " << run + 1 << '/' << RUNS << '\n';

            const int offset = (run + testCase) % 3;

            for (int position = 0; position < 3; ++position) {
                const int algorithm = (position + offset) % 3;

                bool valid = false;
                const double ms = benchmarkOne(
                    algorithm, original, work, valid
                );
                times[testCase][algorithm][run] = ms;

                cout << "  " << left << setw(20)
                     << ALG_NAMES[algorithm]
                     << right << setw(12) << ms << " ms"
                     << " | Sorted: "
                     << (valid ? "yes" : "no")
                     << '\n';

                if (!valid) {
                    cerr << "error: " << ALG_NAMES[algorithm]
                         << " failed on " << CASE_NAMES[testCase]
                         << ", run " << run + 1 << '\n';
                    return 1;
                }
            }
        }
    }

    cout << "Avg results\n";

    cout << left << setw(18) << "Case";

    for (const auto& name : ALG_NAMES)
        cout << right << setw(18) << name;

    cout << '\n';

    for (int testCase = 0; testCase < 6; ++testCase) {
        cout << left << setw(18) << CASE_NAMES[testCase];

        for (int algorithm = 0; algorithm < 3; ++algorithm) {
            double sum = 0.0;

            for (int run = 0; run < RUNS; ++run)
                sum += times[testCase][algorithm][run];

            cout << right << setw(15)
                 << sum / RUNS << " ms";
        }

        cout << '\n';
    }

   
    return 0;
}

