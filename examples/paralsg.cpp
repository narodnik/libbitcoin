#include <bitset>
#include <iterator>
#include <bitcoin/system.hpp>
#include <blake2.h>

namespace bcs = bc::system;

using scalar_list = std::vector<bcs::ec_scalar>;
using value_type = uint64_t;

// FIXME: This is just a random value I picked, not a real generator point
// So we need to calculate a proper value here.
#define LITERAL_H \
"02182f2b3da9f6a8538dabac0e4208bad135e93b8f4824c54f2fa1b974ece63762"

#define PRINT_POINT(name) \
    std::cout << #name " = " << bcs::encode_base16(name.point()) \
        << std::endl;

#define PRINT_SCALAR(name) \
    std::cout << #name " = " << bcs::encode_base16(name.secret()) \
        << std::endl;

const bcs::ec_point ec_point_H = bcs::base16_literal(LITERAL_H);

bcs::ec_scalar random_secret()
{
    bcs::ec_secret secret;
    do
    {
        bcs::pseudo_random::fill(secret);
    } while (!bcs::verify(secret));
    return secret;
}
bcs::ec_scalar random_scalar()
{
    return random_secret();
}

bcs::ec_scalar value_to_scalar(uint64_t value)
{
    auto secret = bcs::null_hash;
    auto serial = bcs::make_unsafe_serializer(secret.end() - 8);
    serial.write_8_bytes_big_endian(value);
    return secret;
}

template <typename ScalarIterator>
bcs::ec_scalar sum(ScalarIterator begin, ScalarIterator end)
{
    if (begin == end)
        return bcs::ec_scalar::zero;
    return *begin + sum(begin + 1, end);
}

struct transaction_rangeproof
{
    // Commitments B_0, B_1, ..., B_n that sum to the blinded
    // pedersen commitment of the value we are proving is >= 0
    bcs::point_list commitments;
    // The actual signature for the commitments that proves each
    // of the 64 digits which make up the value is either 0 or 2^i
    // where i is the i'th ring
    bcs::ring_signature signature;
};

value_type binary_digit_value(size_t index, bool is_set)
{
    if (!is_set)
        return 0;
    return 1 << index;
}

constexpr size_t proof_size = 64;

value_type bitcolumn_value(size_t i)
{
    // Calculate 2^i or what is the value of a binary digit at position i
    return value_type(1) << i;
}

transaction_rangeproof make_rangeproof(
    const value_type value, const bcs::secret_list& secrets,
    const bcs::hash_digest& transaction_hash)
{
    transaction_rangeproof rangeproof;
    bcs::key_rings rings;
    bcs::secret_list salts;

    using value_bitset = std::bitset<proof_size>;
    const value_bitset value_bits(value);

    BITCOIN_ASSERT(secrets.size() == proof_size);

    // Iterate each binary digit of the value
    for (size_t i = 0; i < value_bits.size(); ++i)
    {
        const auto bit_is_set = value_bits[i];
        // Calculate 2^i
        const auto digit_2i = bitcolumn_value(i);

        // Current secret for this binary digit
        const auto& secret = secrets[i];

        // B_i = b_i G
        const auto blind_point = secret * bcs::ec_point::G;
        // V_i = 2^i G
        const auto value_point = value_to_scalar(digit_2i) * ec_point_H;

        // When digit is 0, then C = B_i
        // When digit is 1, then C = B_i + V_i
        auto commitment = blind_point;
        if (bit_is_set)
            commitment += value_point;

        // Sum of all these commitments will equal P
        rangeproof.commitments.push_back(commitment);
        // Secret will be valid for one of the pubkeys in the ring
        // Verifier can recalculate these public keys using just
        // the commitment. They do not need any secret info.
        rings.push_back({ commitment, commitment - value_point });

        // Random s values per each key in the ring.
        // sign() will modify some of these.
        rangeproof.signature.proofs.push_back({
            random_secret(), random_secret() });

        // These are the k values.
        salts.push_back(random_secret());
    }

    bool rc = bcs::sign(rangeproof.signature, secrets, rings,
        transaction_hash, salts);
    BITCOIN_ASSERT(rc);

    return rangeproof;
}

bool verify_rangeproof(const transaction_rangeproof& rangeproof,
    const bcs::hash_digest& transaction_hash)
{
    if (rangeproof.commitments.size() != proof_size)
        return false;

    bcs::key_rings rings;
    for (size_t i = 0; i < proof_size; ++i)
    {
        // Recreate the public keys from the commitments
        const auto& commitment = rangeproof.commitments[i];
        const auto digit_2i = uint64_t(1) << i;
        rings.push_back({
            commitment,
            commitment - value_to_scalar(digit_2i) * ec_point_H
        });
    }

    // Verify the proof.
    return bcs::verify(rings, transaction_hash, rangeproof.signature);
}

std::tuple<bcs::ec_point, bcs::secret_list>
generate_random_blind(value_type value)
{
    auto blinded_value = value_to_scalar(value) * ec_point_H;
    bcs::secret_list secrets;
    for (size_t i = 0; i < proof_size; ++i)
    {
        const auto secret = random_secret();
        secrets.push_back(secret);
        blinded_value += secret * bcs::ec_point::G;
    }
    return std::make_tuple(blinded_value, secrets);
}

template <typename DataType>
bcs::ec_point hash_to_point_impl(const DataType& value)
{
    // Hash input value and coerce to a large number we can increment
    BITCOIN_ASSERT(bcs::hash_size == bcs::ec_secret_size);
    const auto hash = bcs::bitcoin_hash(value);
    const auto& secret = *static_cast<const bcs::ec_secret*>(&hash);

    // Large 32 byte number representing the x value of the point.
    bcs::ec_scalar x_value = secret;

    while (true)
    {
        // Format for a compressed key is 0x02 + [ x_value:32 ]
        bcs::ec_compressed point;
        // Set the first value of the point to 0x02
        point[0] = bcs::ec_point::compressed_even;
        // Copy the x value to the other part of the key
        std::copy(x_value.secret().begin(), x_value.secret().end(),
            point.begin() + 1);

        // Construct an ec_point for this, and test if point is valid
        if (bcs::verify(point))
            return point;

        // Increment and try again until we find valid point on secp curve.
        //x_value += bcs::ec_scalar(1);
        x_value += value_to_scalar(1);
    }

    // Should never reach here!
    return {};
}

bcs::ec_point hash_to_point(const bcs::ec_scalar& scalar)
{
    return hash_to_point_impl(scalar.secret());
}
bcs::ec_point hash_to_point(const bcs::ec_point& point)
{
    return hash_to_point_impl(point.point());
}

using scalar_list = std::vector<bcs::ec_scalar>;
using point_list = std::vector<bcs::ec_point>;
using point_ring = std::vector<point_list>;

bcs::hash_digest digest(
    const bcs::data_slice& message, const point_ring& ring)
{
    const auto sum = [](size_t size, const point_list& ring)
    {
        constexpr auto row_data_size = bcs::ec_compressed_size +
            sizeof(uint32_t) + sizeof(uint32_t);
        return size + ring.size() * row_data_size;
    };

    const auto size = std::accumulate(ring.begin(), ring.end(),
        message.size(), sum);

    bcs::data_chunk data;
    data.reserve(size);
    extend_data(data, message);
    for (size_t i = 0; i < ring.size(); ++i)
        for (size_t j = 0; j < ring[0].size(); ++j)
        {
            bcs::data_chunk row_data;
            auto serial = bcs::make_unsafe_serializer(data.begin());
            serial.write_bytes(ring[i][j].point());
            serial.write_4_bytes_big_endian(i);
            serial.write_4_bytes_big_endian(j);
            bcs::extend_data(data, row_data);
        }

    return bcs::sha256_hash(data);
}

bcs::ec_scalar hash_rings(
    const bcs::data_slice& message,
    const point_ring& left, const point_ring& right)
{
    const auto& rows_size = left.size();
    BITCOIN_ASSERT(rows_size == right.size());
    BITCOIN_ASSERT(rows_size > 0);
    const auto& columns_size = left[0].size();

    constexpr auto row_data_size = bcs::ec_compressed_size +
        sizeof(uint32_t) + sizeof(uint32_t);
    const auto size = message.size() +
        2 * rows_size * columns_size * row_data_size;

    bcs::data_chunk data;
    data.reserve(size);
    extend_data(data, message);

    auto write_item = [&](auto& ring, auto i, auto j)
    {
        bcs::data_chunk row_data(row_data_size);
        auto serial = bcs::make_unsafe_serializer(data.begin());
        serial.write_bytes(ring[i][j].point());
        serial.write_4_bytes_big_endian(i);
        serial.write_4_bytes_big_endian(j);
        bcs::extend_data(data, row_data);
    };

    for (size_t i = 0; i < rows_size; ++i)
        for (size_t j = 0; j < columns_size; ++j)
        {
            write_item(left, i, j);
            write_item(right, i, j);
        }

    return bcs::sha256_hash(data);
}

void borromean_hash2(const bcs::hash_digest& M,
    const point_ring& left, const point_ring& right, uint32_t index,
    bcs::ec_scalar& result)
{
    BITCOIN_ASSERT(left.size() == right.size());
    bcs::data_chunk input_data;
    for (size_t i = 0; i < left.size(); ++i)
    {
        bcs::extend_data(input_data, left[i][index].point());
        bcs::extend_data(input_data, right[i][index].point());
    }

    // e = H(M || R || i )
    bcs::data_chunk data(
        bcs::hash_size + input_data.size() + sizeof(uint32_t));
    auto serial = bcs::make_unsafe_serializer(data.begin());
    serial.write_hash(M);
    serial.write_bytes(input_data);
    serial.write_4_bytes_big_endian(index);
    //return bcs::sha256_hash(data);

    //auto out = blake2s(hash_data, data.data(), NULL,
    //    BLAKE2S_OUTBYTES, data.size(), 0);
    //BITCOIN_ASSERT(out == 0);

    const auto hash = bcs::sha256_hash(data);

    // 11776
    uint8_t* hash_data = const_cast<uint8_t*>(result.secret().data());
    std::copy(hash.data(), hash.data() + hash.size(), hash_data);

    // 36530
    //result = hash;

    //return bcs::ec_scalar::zero;
    //return bcs::null_hash;
    //bcs::hash_digest foo;
    //std::copy(bcs::null_hash.begin(), bcs::null_hash.end(),
    //    foo.begin());
    //result = foo;
}

bcs::ec_scalar borromean_hash(const bcs::hash_digest& M,
    const point_ring& left, const point_ring& right, uint32_t index)
{
    BITCOIN_ASSERT(left.size() == right.size());
    bcs::data_chunk input_data;
    for (size_t i = 0; i < left.size(); ++i)
    {
        bcs::extend_data(input_data, left[i][index].point());
        bcs::extend_data(input_data, right[i][index].point());
    }

    // e = H(M || R || i )
    bcs::data_chunk data(
        bcs::hash_size + input_data.size() + sizeof(uint32_t));
    auto serial = bcs::make_unsafe_serializer(data.begin());
    serial.write_hash(M);
    serial.write_bytes(input_data);
    serial.write_4_bytes_big_endian(index);
    return bcs::sha256_hash(data);
}

struct mlsag_signature
{
    using scalar_table = std::vector<scalar_list>;

    point_list key_images;
    scalar_list challenges;
    scalar_table salts;
};

auto create_ring(auto rows, auto columns)
{
    return point_ring(rows, point_list(columns));
}

auto sum_all(const auto& challenges)
{
    bcs::ec_scalar result = bcs::ec_scalar::zero;
    for (const auto& challenge: challenges)
        result += challenge;
    return result;
}

mlsag_signature mlsag_sign(const scalar_list& secrets,
    const point_ring& publics, const size_t index)
{
    const auto& G = bcs::ec_point::G;
    mlsag_signature signature;

    const auto rows_size = publics.size();
    BITCOIN_ASSERT(secrets.size() == rows_size);
    BITCOIN_ASSERT(rows_size > 0);
    const auto columns_size = publics[0].size();
    BITCOIN_ASSERT(index < columns_size);

    // Our 'response' values.
    using scalar_table = std::vector<scalar_list>;
    // random s values
    signature.salts = scalar_table(rows_size, scalar_list(columns_size));
    for (auto& column: signature.salts)
        std::generate(column.begin(), column.end(), random_scalar);

    // Hash every public key, put it in a table.
    auto hashed_publics = create_ring(rows_size, columns_size);
    for (size_t i = 0; i < rows_size; ++i)
        for (size_t j = 0; j < columns_size; ++j)
            // H_p(P)
            hashed_publics[i][j] = hash_to_point(publics[i][j]);

    // Now create the L and R values.
    auto left_points = create_ring(rows_size, columns_size);
    auto right_points = create_ring(rows_size, columns_size);

    // Compute the starting L, R value for our key
    for (size_t i = 0; i < rows_size; ++i)
    {
        // L = k G
        left_points[i][index] = signature.salts[i][index] * G;
        // R = k H_p(P = x G)
        right_points[i][index] =
            signature.salts[i][index] * hashed_publics[i][index];
    }

    // Move to next challenge for the next row
    auto j = (index + 1) % columns_size;
    // Calculate first challenge value based off H(kG, kH(P))
    auto& challenges = signature.challenges;
    challenges.resize(columns_size);
    std::generate(challenges.begin(), challenges.end(), random_scalar);

    for (const auto secret: secrets)
    {
        BITCOIN_ASSERT(bcs::verify(secret.secret()));
        // I = x H_p(P = x G)
        const auto image = secret * hash_to_point(secret * G);
        signature.key_images.push_back(image);
    }

    /////// Initialization, done.

    std::cout << "Now performing signature..." << std::endl;

    for (size_t j = 0; j < columns_size; ++j)
    {
        //std::cout << j << "... " << std::endl;

        if (j == index)
            continue;

        for (size_t i = 0; i < rows_size; ++i)
        {
            // L = sG + cP
            left_points[i][j] =
                signature.salts[i][j] * G + challenges[j] * publics[i][j];
            // R = sH(P) + cI
            right_points[i][j] =
                signature.salts[i][j] * hashed_publics[i][j] +
                challenges[j] * signature.key_images[i];

            //PRINT_POINT(left_points[i][j]);
            //PRINT_POINT(right_points[i][j]);
            //PRINT_SCALAR(signature.salts[i][j]);
            //PRINT_POINT(publics[i][j]);
            //PRINT_POINT(hashed_publics[i][j]);
            //PRINT_POINT(signature.key_images[i]);
        }
        //PRINT_SCALAR(challenges[j]);

        if (j % 400 == 0)
            std::cout << j << "... " << std::flush;
    }
    std::cout << std::endl;

    // Hash all the available keys into a value we'll use
    // when hashing the challenges.
    const auto total_challenge = hash_rings(
        bcs::base16_literal("deadbeef"), left_points, right_points);
    PRINT_SCALAR(total_challenge);

    auto sum_except_i = [](const auto& challenges, size_t index)
    {
        bcs::ec_scalar result = bcs::ec_scalar::zero;
        for (size_t i = 0; i < challenges.size(); ++i)
        {
            if (i == index)
                continue;
            result += challenges[i];
        }
        return result;
    };

    challenges[index] = total_challenge - sum_except_i(challenges, index);

    BITCOIN_ASSERT(sum_all(challenges) == total_challenge);

    // Now close the ring by calculating the correct salt at index
    std::cout << "Setting s for index = " << index << std::endl;
    for (size_t i = 0; i < rows_size; ++i)
    {
        signature.salts[i][index] =
            signature.salts[i][index] - challenges[index] * secrets[i];
        PRINT_SCALAR(signature.salts[i][index]);

        BITCOIN_ASSERT(left_points[i][index] ==
            signature.salts[i][index] * G +
            challenges[index] * publics[i][index]);
    }

    return signature;
}

bool mlsag_verify(const point_ring& publics, const mlsag_signature& signature)
{
    const auto& G = bcs::ec_point::G;

    const auto rows_size = publics.size();
    BITCOIN_ASSERT(rows_size > 0);
    const auto columns_size = publics[0].size();

    // Hash every public key, put it in a table.
    auto hashed_publics = create_ring(rows_size, columns_size);
    for (size_t i = 0; i < rows_size; ++i)
        for (size_t j = 0; j < columns_size; ++j)
        {
            // H_p(P)
            hashed_publics[i][j] = hash_to_point(publics[i][j]);
            //PRINT_POINT(hashed_publics[i][j]);
        }

    // Create the L and R values.
    auto left_points = create_ring(rows_size, columns_size);
    auto right_points = create_ring(rows_size, columns_size);

    const auto& challenges = signature.challenges;

    std::cout << "Verifying signature..." << std::endl;
    for (size_t j = 0; j < columns_size; ++j)
    {
        //std::cout << j << "... " << std::endl;

        for (size_t i = 0; i < rows_size; ++i)
        {
            // L = sG + cP
            left_points[i][j] =
                signature.salts[i][j] * G + challenges[j] * publics[i][j];
            // R = sH(P) + cI
            right_points[i][j] =
                signature.salts[i][j] * hashed_publics[i][j] +
                challenges[j] * signature.key_images[i];

            //PRINT_POINT(left_points[i][j]);
            //PRINT_POINT(right_points[i][j]);
            //PRINT_SCALAR(signature.salts[i][j]);
            //PRINT_POINT(publics[i][j]);
            //PRINT_POINT(hashed_publics[i][j]);
            //PRINT_POINT(signature.key_images[i]);
        }
        //PRINT_SCALAR(challenges[j]);

        if (j % 100 == 0)
            std::cout << j << "... " << std::flush;
    }
    std::cout << std::endl;

    // Hash all the available keys into a value we'll use
    // when hashing the challenges.
    const auto total_challenge = hash_rings(
        bcs::base16_literal("deadbeef"), left_points, right_points);
    PRINT_SCALAR(total_challenge);

    return sum_all(challenges) == total_challenge;
}

void ring_ct_simple()
{
    const auto& G = bcs::ec_point::G;
    //const auto H = hash_to_point(bcs::ec_scalar(0xdeadbeef));
    const auto H = hash_to_point(value_to_scalar(0xdeadbeef));

    PRINT_POINT(G);
    PRINT_POINT(H);
    std::cout << std::endl;

#define BLIND_A \
    "174ff68c2a964701642e343a0a0fc3437e5c2d7242d150d0173ec006fbd900b7"
#define BLIND_B \
    "41e146a7bb895fcdbb7ab6b33c598b5693be6480455f878964f45fdac7266393"
#define BLIND_C \
    "027338898dd3e3bc42b1da0c1b4dbfa1989cef8afb9dbe6960015c5f83f11aef"

    // Input values
    const bcs::ec_scalar blind_a{ bcs::base16_literal(BLIND_A) };
    //const bcs::ec_scalar value_a(10000);
    const auto value_a = value_to_scalar(10000);
    const auto commit_a = blind_a * G + value_a * H;

    PRINT_SCALAR(blind_a);
    PRINT_SCALAR(value_a);
    PRINT_POINT(commit_a);
    std::cout << std::endl;

    // Output values
    const bcs::ec_scalar blind_b{ bcs::base16_literal(BLIND_B) };
    //const bcs::ec_scalar value_b(7000);
    const auto value_b = value_to_scalar(7000);
    const auto commit_b = blind_b * G + value_b * H;

    PRINT_SCALAR(blind_b);
    PRINT_SCALAR(value_b);
    PRINT_POINT(commit_b);
    std::cout << std::endl;

    const bcs::ec_scalar blind_c{ bcs::base16_literal(BLIND_C) };
    //const bcs::ec_scalar value_c(3000);
    const auto value_c = value_to_scalar(3000);
    const auto commit_c = blind_c * G + value_c * H;

    PRINT_SCALAR(blind_c);
    PRINT_SCALAR(value_c);
    PRINT_POINT(commit_c);
    std::cout << std::endl;

#define PRIVATE_KEY \
    "6184aee9c77893796f3c780ea43db9de8dfa24f1df5260f4acb148f0c6a7609f"

    const bcs::ec_scalar private_key{
        bcs::base16_literal(PRIVATE_KEY) };
    const auto public_key = private_key * G;

    PRINT_SCALAR(private_key);
    PRINT_POINT(public_key);
    std::cout << std::endl;

#if 0
    const auto decoy_public_key =
        hash_to_point(bcs::ec_scalar(110));
    const auto decoy_commit =
        hash_to_point(bcs::ec_scalar(4));
    PRINT_POINT(decoy_public_key);
    PRINT_POINT(decoy_commit);
    std::cout << std::endl;
#endif

    const auto commitment_secret = blind_a - (blind_b + blind_c);
    const auto output_commit = commit_b + commit_c;

    const scalar_list secrets{ private_key, commitment_secret };
    point_ring publics{
        { public_key }, { commit_a - output_commit } };
    const auto index = 0;

    std::cout << "Generating decoys..." << std::endl;
    for (size_t i = 0; i < 100'000; ++i)
    {
        const auto decoy_public_key =
            //hash_to_point(bcs::ec_scalar(i + 110));
            hash_to_point(value_to_scalar(i + 110));
        const auto decoy_commit =
            //hash_to_point(bcs::ec_scalar(i + 4));
            hash_to_point(value_to_scalar(i + 4));

        publics[0].push_back(decoy_public_key);
        publics[1].push_back(decoy_commit - output_commit);

        if (i % 100 == 0)
            std::cout << i << "... ";
    }
    std::cout << std::endl;

    mlsag_signature signature;
    bcs::timer time;
    auto duration = time.execution([&]
        {
            signature = mlsag_sign(secrets, publics, index);
        });
    std::cout << "Sign took: " << duration << " ms" << std::endl;
    //auto signature = mlsag_sign(secrets, publics, index);
    duration = time.execution([&]
        {
            auto success = mlsag_verify(publics, signature);
            BITCOIN_ASSERT(success);
        });
    std::cout << "Verify took: " << duration << " ms" << std::endl;

}

void make_bazillion_decoys(auto n)
{
    for (size_t i = 0; i < n; ++i)
    {
        const auto decoy_public_key =
            hash_to_point(bcs::ec_scalar(110));
        const auto decoy_commit =
            hash_to_point(bcs::ec_scalar(4));

        if (i % 100 == 0)
            std::cout << i << "... ";
    }
    std::cout << std::endl;
}

int main()
{
    //make_bazillion_decoys(100'000);
    ring_ct_simple();

    //bcs::ec_scalar foo(110);
    //auto g = hash_to_point(foo);
    //bcs::ec_scalar bar = std::move(foo);
    return 0;

    /////// Rangeproof code ////////

    // We make a rangeproof to prove value >= 0 within 64 bits
    // Given P = b G + v H
    // v = value
    // b = blind
    // G and H are generator points
    // P = pedersen commit that encodes the hidden value
    value_type value = 110;
    bcs::hash_digest transaction_hash = bcs::null_hash;

    // The below in C++17 becomes simply:
    //   const auto [blinded_value, secrets] = generate_random_blind(value);
    bcs::ec_point blinded_value;
    bcs::secret_list secrets;
    std::tie(blinded_value, secrets) = generate_random_blind(value);

    // Prover makes the rangeproof
    auto rangeproof = make_rangeproof(value, secrets, transaction_hash);

    // Verifier checks the commitments are valid
    // They must sum to the same blinded point P they are provided.
    bcs::ec_compressed result;
    bcs::ec_sum(result, rangeproof.commitments);
    BITCOIN_ASSERT(blinded_value.point() == result);

    // Verifier now verifies the proof, that for the
    // 64 values of B_0, B_1, ..., B_n where sum(B_i) == P
    // that each one is either B_i = b G + 0 H, OR B_i = b G + 2^i H
    // This is the rangeproof.
    bool success = verify_rangeproof(rangeproof, transaction_hash);
    BITCOIN_ASSERT(success);
    std::cout << "Passed." << std::endl;
    return 0;
}

