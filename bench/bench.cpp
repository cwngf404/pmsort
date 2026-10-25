#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "pmsort.hpp"

using namespace std;
using Clock = chrono::steady_clock;

constexpr size_t N = 10'000'000;
constexpr int RUNS = 5;
constexpr size_t NEARLY_SWAPS = N / 1000;

constexpr uint32_t SEED = 4042026;
constexpr int DUP_RANGE = 10'000;
constexpr int ZIPF_RANGE = 100'000;
constexpr double ZIPF_EXPONENT = 1.2;

constexpr int ALGORITHM_COUNT = 3;
constexpr int CASE_COUNT = 6;

enum Algorithm {
    PMSORT = 0,
    STD_SORT = 1,
    STABLE_SORT = 2
};

const array<string, ALGORITHM_COUNT> ALG_NAMES = {
    "pmsort",
    "std::sort",
    "std::stable_sort"
};

const array<string, CASE_COUNT> CASE_NAMES = {
    "Random",
    "Duplicates",
    "Nearly sorted",
    "Pipe organ",
    "Reverse",
    "Zipf"
};

mt19937 rng(SEED);
mt19937_64 rng64(SEED);
mt19937 orderRng(SEED ^ 0x4042026u);

// build the zipf cumulative distribution
vector<double> buildZipfCDF() {
    vector<double> cdf(ZIPF_RANGE);
    double total = 0.0;

    for (int i = 1; i <= ZIPF_RANGE; ++i) {
        total += 1.0 / pow(
            static_cast<double>(i),
            ZIPF_EXPONENT
        );

        cdf[i - 1] = total;
    }

    for (double& x : cdf)
        x /= total;

    return cdf;
}

int sampleZipf(
    const vector<double>& cdf,
    uniform_real_distribution<double>& dist
) {
    double x = dist(rng64);

    auto it = lower_bound(cdf.begin(), cdf.end(), x);

    return static_cast<int>(it - cdf.begin()) + 1;
}

// generate benchmark data
vector<int> generateData(
    int type,
    const vector<double>& zipfCDF
) {
    vector<int> a(N);

    switch (type) {
    case 0: {
        uniform_int_distribution<int> dist(
            1, 1'000'000'000
        );

        for (int& x : a)
            x = dist(rng);

        break;
    }

    case 1: {
        uniform_int_distribution<int> dist(
            1, DUP_RANGE
        );

        for (int& x : a)
            x = dist(rng);

        break;
    }

    case 2: {
        iota(a.begin(), a.end(), 0);

        uniform_int_distribution<size_t> pos(
            0, N - 1
        );

        for (size_t i = 0; i < NEARLY_SWAPS; ++i) {
            size_t x = pos(rng);
            size_t y = pos(rng);

            swap(a[x], a[y]);
        }

        break;
    }

    case 3: {
        for (size_t i = 0; i < N; ++i) {
            a[i] = static_cast<int>(
                min(i, N - 1 - i)
            );
        }

        break;
    }

    case 4: {
        iota(a.begin(), a.end(), 0);
        reverse(a.begin(), a.end());

        break;
    }

    case 5: {
        uniform_real_distribution<double> dist(
            0.0, 1.0
        );

        for (int& x : a)
            x = sampleZipf(zipfCDF, dist);

        break;
    }
    }

    return a;
}

// run the selected sorting algorithm
void runSort(int algorithm, vector<int>& a) {
    switch (algorithm) {
    case PMSORT:
        pmsort(a.begin(), a.end(), less<int>{});
        break;

    case STD_SORT:
        sort(a.begin(), a.end());
        break;

    case STABLE_SORT:
        stable_sort(a.begin(), a.end());
        break;
    }
}

struct Result {
    double ms = 0.0;
    bool correct = false;
};

// benchmark sorting time and validate the result
Result benchmarkOne(
    int algorithm,
    const vector<int>& original,
    const vector<int>& expected,
    vector<int>& work
) {
    work = original;

    auto start = Clock::now();

    runSort(algorithm, work);

    auto end = Clock::now();

    Result result;

    result.ms = chrono::duration<double, milli>(
        end - start
    ).count();

    result.correct =
        is_sorted(work.begin(), work.end()) &&
        work == expected;

    return result;
}

// calculate average execution time
double average(const array<double, RUNS>& values) {
    double sum = 0.0;

    for (double x : values)
        sum += x;

    return sum / RUNS;
}

// calculate population standard deviation
double stddev(const array<double, RUNS>& values) {
    double mean = average(values);
    double sum = 0.0;

    for (double x : values) {
        double diff = x - mean;
        sum += diff * diff;
    }

    return sqrt(sum / RUNS);
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    cout << fixed << setprecision(3);

    cout << "pms sorting benchmark\n";
    cout << "elements: " << N << '\n';
    cout << "runs: " << RUNS << '\n';
    cout << "seed: " << SEED << '\n';
    cout << "nearly sorted swaps: " << NEARLY_SWAPS << '\n';

    const vector<double> zipfCDF = buildZipfCDF();

    vector<int> work;

    // warm up each algorithm using the same dataset
    const vector<int> warmupOriginal = generateData(0, zipfCDF);
    vector<int> warmupExpected = warmupOriginal;

    sort(warmupExpected.begin(), warmupExpected.end());

    for (int algorithm = 0;
         algorithm < ALGORITHM_COUNT;
         ++algorithm) {

        Result result = benchmarkOne(
            algorithm,
            warmupOriginal,
            warmupExpected,
            work
        );

        if (!result.correct) {
            cerr << "error: warmup validation failed for "
                 << ALG_NAMES[algorithm] << '\n';

            return 1;
        }
    }

    vector<int>().swap(work);

    // store execution times for every case and algorithm
    array<
        array<array<double, RUNS>, ALGORITHM_COUNT>,
        CASE_COUNT
    > times{};

    for (int testCase = 0;
         testCase < CASE_COUNT;
         ++testCase) {

        const vector<int> original =
            generateData(testCase, zipfCDF);

        vector<int> expected = original;
        sort(expected.begin(), expected.end());

        for (int run = 0; run < RUNS; ++run) {
            array<int, ALGORITHM_COUNT> order = {
                PMSORT,
                STD_SORT,
                STABLE_SORT
            };

            shuffle(
                order.begin(),
                order.end(),
                orderRng
            );

            for (int algorithm : order) {
                Result result = benchmarkOne(
                    algorithm,
                    original,
                    expected,
                    work
                );

                if (!result.correct) {
                    cerr << "error: validation failed\n"
                         << "case: " << CASE_NAMES[testCase] << '\n'
                         << "algorithm: "
                         << ALG_NAMES[algorithm] << '\n'
                         << "run: " << run + 1 << '\n';

                    return 1;
                }

                times[testCase][algorithm][run] =
                    result.ms;
            }
        }
    }

    // print average execution times
    cout << "\naverage results (ms)\n";

    cout << left << setw(20) << "case";

    for (const string& name : ALG_NAMES)
        cout << right << setw(20) << name;

    cout << '\n';

    for (int testCase = 0;
         testCase < CASE_COUNT;
         ++testCase) {

        cout << left << setw(20)
             << CASE_NAMES[testCase];

        for (int algorithm = 0;
             algorithm < ALGORITHM_COUNT;
             ++algorithm) {

            cout << right << setw(17)
                 << average(times[testCase][algorithm])
                 << " ms";
        }

        cout << '\n';
    }

    // print execution time standard deviations
    cout << "\nstandard deviation (ms)\n";

    cout << left << setw(20) << "case";

    for (const string& name : ALG_NAMES)
        cout << right << setw(20) << name;

    cout << '\n';

    for (int testCase = 0;
         testCase < CASE_COUNT;
         ++testCase) {

        cout << left << setw(20)
             << CASE_NAMES[testCase];

        for (int algorithm = 0;
             algorithm < ALGORITHM_COUNT;
             ++algorithm) {

            cout << right << setw(17)
                 << stddev(times[testCase][algorithm])
                 << " ms";
        }

        cout << '\n';
    }

    cout << "\nvalidation: all passed\n";

    return 0;
}
