#ifndef _PDR_WEIGHTED_APPROXIMATION_BASED_REFACTOR_HPP
#define _PDR_WEIGHTED_APPROXIMATION_BASED_REFACTOR_HPP

#include "RefactorInterface.hpp"
#include "PDR/Approximator/Approximator.hpp"
#include "MDR/BitplaneEncoder/BitplaneEncoder.hpp"
#include "MDR/LosslessCompressor/LevelCompressor.hpp"
#include "MDR/Writer/Writer.hpp"
#include "MDR/RefactorUtils.hpp"
#include "WeightUtils.hpp"
#include "ompSZp_typemanager.c"

using namespace MDR;

namespace PDR {

    // an approximation-based scientific data refactor: compose an approximation algorithm, encoder, and lossless compressor
    template<class T, class Approximator, class Encoder, class Compressor, class Writer>
    class WeightedApproximationBasedRefactor : public concepts::RefactorInterface<T> {
    public:
        WeightedApproximationBasedRefactor(Approximator approximator, Encoder encoder, Compressor compressor, Writer writer)
            : approximator(approximator), encoder(encoder), compressor(compressor), writer(writer) {}

        void refactor(T const * data_, const std::vector<uint32_t>& dims, uint8_t target_level, uint8_t num_bitplanes)
        {

        }

        void refactor(T const * data_, const std::vector<uint32_t>& dims, uint8_t target_level, uint8_t num_bitplanes, int max_weight = 4, const int block_size = 1){
            // Timer timer;
            // timer.start();
            dimensions = dims;
            uint32_t num_elements = 1;
            for(const auto& dim:dimensions){
                num_elements *= dim;
            }
            data = std::vector<T>(data_, data_ + num_elements);
            weights = QoI;
            // if refactor successfully
            if(refactor(num_bitplanes, max_weight, block_size)){
                // timer.end();
                // timer.print("Refactor");
                // timer.start();
                level_num = writer.write_level_components(level_components, level_sizes);
                // timer.end();
                // timer.print("Write");                
            }

            if(store_weight) write_weight(block_size);
            write_metadata();
            for(int i=0; i<level_components.size(); i++){
                for(int j=0; j<level_components[i].size(); j++){
                    free(level_components[i][j]);                    
                }
            }
        }

        void write_metadata() const {
            uint32_t metadata_size = sizeof(uint8_t) + get_size(dimensions) // dimensions
                            + sizeof(size_t) + sizeof(size_t)
                            + sizeof(uint8_t) + get_size(level_error_bounds) 
                            + get_size(level_sizes) // level information
                            + get_size(stopping_indices) + get_size(level_num) + 1 + sizeof(T); // one byte for whether negabinary encoding is used 
            uint8_t * metadata = (uint8_t *) malloc(metadata_size);
            uint8_t * metadata_pos = metadata;
            *(metadata_pos ++) = (uint8_t) dimensions.size();
            serialize(dimensions, metadata_pos);
            *reinterpret_cast<size_t*>(metadata_pos) = approximator_size;
            metadata_pos += sizeof(size_t);
            *reinterpret_cast<size_t*>(metadata_pos) = weight_file_size;
            metadata_pos += sizeof(size_t);
            *(metadata_pos ++) = (uint8_t) 1; // level = 1
            serialize(level_error_bounds, metadata_pos);
            serialize(level_sizes, metadata_pos);
            serialize(stopping_indices, metadata_pos);
            serialize(level_num, metadata_pos);
            *(metadata_pos ++) = (uint8_t) negabinary;
            *reinterpret_cast<T*>(metadata_pos) = approximator_eb;
            writer.write_metadata(metadata, metadata_size);
            free(metadata);
        }

        void write_weight(const int block_size) const {
            // {
            //     int max_w = block_weights[0];
            //     int min_w = block_weights[0];
            //     for(int i=1; i< block_weights.size(); i++){
            //         if(block_weights[i] > max_w) max_w = block_weights[i];
            //         if(block_weights[i] < min_w) min_w = block_weights[i];
            //     }
            //     std::cout << min_w << " " << max_w << std::endl;
            // }
 
            uint32_t weight_size = sizeof(int) + sizeof(unsigned int) + sizeof(size_t) + sizeof(uint32_t) + ZSTD_weight_size;
            uint8_t * weight_data = (uint8_t *) malloc(weight_size);
            uint8_t * weight_data_pos = weight_data;
            memcpy(weight_data_pos, &block_size, sizeof(int));
            weight_data_pos += sizeof(int);
            memcpy(weight_data_pos, &bit_count, sizeof(unsigned int));
            weight_data_pos += sizeof(unsigned int);
            size_t intArrayLength = block_weights.size();
            memcpy(weight_data_pos, &intArrayLength, sizeof(size_t));
            weight_data_pos += sizeof(size_t);
            memcpy(weight_data_pos, &ZSTD_weight_size, sizeof(uint32_t));  
            weight_data_pos += sizeof(uint32_t);
            memcpy(weight_data_pos, ZSTD_weights, ZSTD_weight_size);
            string path = writer.get_directory() + "weight.bin";
            // std::cout << "Path: " << path << std::endl;
            FILE * file = fopen(path.c_str(), "wb");
            if (file == nullptr) {
                perror("Error opening file");
                return;
            }
            fwrite(weight_data, 1, weight_size, file);
            fclose(file);
            free(weight_data);
            free(ZSTD_weights);
            weight_file_size = static_cast<size_t>(weight_size);
        }

        void write_weight_dat(const int block_size) const {
            uint32_t weight_size = get_size(int_weights);
            uint8_t * weight_data = (uint8_t *) malloc(weight_size);
            uint8_t * weight_data_pos = weight_data;
            serialize(int_weights, weight_data_pos);
            string path = writer.get_directory() + "weight.dat";
            // std::cout << "Path: " << path << std::endl;
            FILE * file = fopen(path.c_str(), "w");
            if (file == nullptr) {
                perror("Error opening file");
                return;
            }
            fwrite(weight_data, 1, weight_size, file);
            fclose(file);
            free(weight_data);
        }

        std::vector<int> get_int_weights(){
            return int_weights;
        }

        void copy_int_weights(std::vector<int> & completed_weights){
            int_weights = completed_weights;
        }

        ~WeightedApproximationBasedRefactor(){}

        void print() const {
            std::cout << "Approximation-based refactor with the following components." << std::endl;
            std::cout << "Approximator: "; approximator.print();
            std::cout << "Encoder: "; encoder.print();
        }
    private:
        bool refactor(uint8_t num_bitplanes, int max_weight, const int block_size){
            if(store_weight){
                if (dimensions.size() == 1){
                    assign_block_value_1D(dimensions[0], block_size, weights.data());
                }
                else if (dimensions.size() == 2){
                    assign_block_value_2D(dimensions[0], dimensions[1], dimensions[1], block_size, weights.data());
                }
                else if (dimensions.size() == 3){
                    assign_block_value_3D(dimensions[0], dimensions[1], dimensions[2], dimensions[1]*dimensions[2], dimensions[2], block_size, weights.data());
                }
                int_weights = normalize_weights(weights, max_weight);
                if(dimensions.size() == 1){
                    block_weights = get_block_weight_1D(dimensions[0], int_weights, block_size);
                }
                else if(dimensions.size() == 2){
                    block_weights = get_block_weight_2D(dimensions[0], dimensions[1], int_weights, block_size);
                }
                else{
                    block_weights = get_block_weight_3D(dimensions[0], dimensions[1], dimensions[2], int_weights, block_size);
                }
                // calculate the space that compressed_weights needs and resize it since we are passing the pointer
                bit_count = static_cast<unsigned int>(std::ceil(std::log2(max_weight + 1)));
                unsigned int byte_count = bit_count / 8; // calculate the byte_count
                unsigned int remainder_bit = bit_count % 8;
                size_t byteLength = 0;
                if (remainder_bit == 0) {
                    byteLength = byte_count * block_weights.size() + 1;
                } 
                else {
                    size_t tmp = remainder_bit * block_weights.size();
                    byteLength = byte_count * block_weights.size() + (tmp - 1) / 8 + 1;
                }
                // std::cout << "bit_count = " << static_cast<size_t>(bit_count) << " byte_count = " << static_cast<size_t>(byte_count) << " remainder_bit = " << static_cast<size_t>(remainder_bit) << " byteLength = " << byteLength << " block_weights.size() = " << block_weights.size() << std::endl;
                compressed_weights.resize(byteLength);
                if (byteLength != anonymous_save_fixed_length_bits(reinterpret_cast<unsigned int*>(block_weights.data()), block_weights.size(), compressed_weights.data(), bit_count)){
                    // perror("From WeightedApproximationBasedRefactor: Error: byteLength != weight_size\n");
                }
                ZSTD_weight_size = ZSTD::compress(compressed_weights.data(), byteLength, &ZSTD_weights);
            }

            auto num_elements = data.size();
            T max_val = data[0];
            T min_val = data[0];
            for(int i=1; i<num_elements; i++){
                if(data[i] > max_val) max_val = data[i];
                if(data[i] < min_val) min_val = data[i];
            }
            approximator_eb *= (max_val - min_val);
            std::string approximator_path = writer.get_directory() + "approximator.dat";
            approximator_size = approximator.refactor_approximate(data.data(), dimensions, approximator_eb, approximator_path);

            level_error_bounds.clear();
            level_components.clear();
            level_sizes.clear();
            
            if (mask.empty()){
                T level_max_error = compute_max_abs_value(data.data(), num_elements);
                int level_max_weight = compute_max_abs_value(int_weights.data(), num_elements);
                // encoding
                if(negabinary) level_error_bounds.push_back(level_max_error * 4);
                else level_error_bounds.push_back(level_max_error);
                int level_exp = 0;
                frexp(level_max_error, &level_exp);
                std::vector<uint32_t> stream_sizes;
                auto streams = encoder.encode(data.data(), num_elements, level_exp + level_max_weight, num_bitplanes, stream_sizes, int_weights.data());

                // lossless
                uint8_t stopping_index = compressor.compress_level(streams, stream_sizes);
                stopping_indices.push_back(stopping_index);
                level_components.push_back(streams);
                level_sizes.push_back(stream_sizes);
            }
            else{
                int num_valid_data = std::accumulate(mask.begin(), mask.end(), 0);
                std::vector<T> filtered_data(num_valid_data);
                std::vector<int> filtered_int_weights(num_valid_data);
                int filtered_index = 0;
                for(int i=0; i<mask.size(); i++){
                    if(mask[i]){
                        filtered_data[filtered_index]=data[i];
                        filtered_int_weights[filtered_index++]=int_weights[i];
                    }
                }
                T level_max_error = compute_max_abs_value(filtered_data.data(), num_valid_data);
                int level_max_weight = compute_max_abs_value(filtered_int_weights.data(), num_valid_data);
                // encoding
                if(negabinary) level_error_bounds.push_back(level_max_error * 4);
                else level_error_bounds.push_back(level_max_error);
                int level_exp = 0;
                frexp(level_max_error, &level_exp);
                std::vector<uint32_t> stream_sizes;
                auto streams = encoder.encode(filtered_data.data(), num_valid_data, level_exp + level_max_weight, num_bitplanes, stream_sizes, filtered_int_weights.data());

                // lossless
                uint8_t stopping_index = compressor.compress_level(streams, stream_sizes);
                stopping_indices.push_back(stopping_index);
                level_components.push_back(streams);
                level_sizes.push_back(stream_sizes);
            }

            return true;
        }

        Approximator approximator;
        Encoder encoder;
        Compressor compressor;
        Writer writer;
        unsigned int bit_count;
        size_t approximator_size = 0;
        mutable size_t weight_file_size = 0;
        uint8_t* ZSTD_weights = nullptr;
        uint32_t ZSTD_weight_size;
        std::vector<T> data;
        std::vector<T> weights;
        std::vector<int> int_weights;
        std::vector<int> block_weights;
        std::vector<unsigned char> compressed_weights;
        std::vector<uint32_t> dimensions;
        T approximator_eb = 0.001;
        std::vector<T> level_error_bounds;
        std::vector<uint8_t> stopping_indices;
        std::vector<std::vector<uint8_t*>> level_components;
        std::vector<uint32_t> level_num;
        std::vector<std::vector<uint32_t>> level_sizes;
    public:
        bool negabinary = false;
        bool store_weight = false;
        std::vector<T> QoI;
        std::vector<unsigned char> mask;
    };
}
#endif

