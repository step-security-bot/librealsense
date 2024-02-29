// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2024 Intel Corporation. All Rights Reserved.

#include <src/fw-logs/fw-logs-parser.h>
#include <src/fw-logs/fw-logs-xml-helper.h>

#include <rsutils/string/from.h>

#include <regex>
#include <fstream>
#include <iterator>


namespace librealsense
{
    namespace fw_logs
    {
        fw_logs_parser::fw_logs_parser( const std::string & definitions_xml )
            : _source_to_formatting_options()
        {
            // The definitions XML should contain entries for all log sources.
            // For each source it lists parser options file path and (optional) module verbosity level.
            fw_logs_xml_helper helper;
            _source_id_to_name = helper.get_listed_sources( definitions_xml );
            for( const auto & source : _source_id_to_name )
            {
                if( source.first >= fw_logs::max_sources )
                    throw librealsense::invalid_value_exception( rsutils::string::from()
                                                                 << "Supporting source id 0 to " << fw_logs::max_sources
                                                                 << ". Found source (" << source.first << ", "
                                                                 << source.second << ")" );

                std::string path = helper.get_source_parser_file_path( source.first, definitions_xml );
                std::ifstream f( path.c_str() );
                if( f.good() )
                {
                    std::string xml_contents;
                    xml_contents.append( std::istreambuf_iterator< char >( f ), std::istreambuf_iterator< char >() );
                    fw_logs_formatting_options format_options( std::move( xml_contents ) );
                    _source_to_formatting_options[source.first] = format_options;
                }
                else
                    throw librealsense::invalid_value_exception( rsutils::string::from() << "Can't open file " << path );

                auto verbosity = helper.get_source_module_verbosity( source.first, definitions_xml );
                if( !verbosity.empty() && verbosity.rbegin()->first >= fw_logs::max_modules )
                    throw librealsense::invalid_value_exception( rsutils::string::from()
                                                                 << "Supporting module id 0 to " << fw_logs::max_modules
                                                                 << ". Found module " << verbosity.rbegin()->first
                                                                 << " in source (" << source.first << ", "
                                                                 << source.second << ")" );

                _verbosity_settings.module_filter[source.first] = 0;
                for( const auto & module : verbosity )
                {
                    // Each bit maps to one module. If the bit is 1 logs from this module shall be collected.
                    _verbosity_settings.module_filter[source.first] |= module.second ? 1 << module.first : 0;
                    // Each item maps to one SW module. Only logs of that severity level will be collected.
                    _verbosity_settings.severity_level[source.first][module.first] = module.second;
                }
            }
        }

        fw_log_data fw_logs_parser::parse_fw_log( const fw_logs_binary_data * fw_log_msg ) 
        {
            fw_log_data parsed_data = fw_log_data();

            if( ! fw_log_msg || fw_log_msg->logs_buffer.size() == 0 )
                return parsed_data;

            // Struct data in a common format
            structured_binary_data structured = structure_common_data( fw_log_msg );
            structure_timestamp( fw_log_msg, &structured );

            const fw_logs_formatting_options & formatting = get_format_options_ref( structured.source_id );

            kvp event_data = formatting.get_event_data( structured.event_id );
            structure_params( fw_log_msg, event_data.first, &structured );

            // Parse data
            fw_string_formatter reg_exp( formatting.get_enums() );
            parsed_data.message = reg_exp.generate_message( event_data.second,
                                                            structured.params_info,
                                                            structured.params_blob );
            
            parsed_data.line = structured.line;
            parsed_data.sequence = structured.sequence;
            parsed_data.timestamp = structured.timestamp;
            parsed_data.severity = parse_severity( structured.severity );
            parsed_data.source_name = get_source_name( structured.source_id );
            parsed_data.file_name = formatting.get_file_name( structured.file_id );
            parsed_data.module_name = formatting.get_module_name( structured.module_id );

            return parsed_data;
        }

        size_t fw_logs_parser::get_log_size( const uint8_t * log ) const
        {
            const auto extended = reinterpret_cast< const extended_fw_log_binary * >( log );
            // If there are parameters total_params_size_bytes includes the information
            //return sizeof( extended_fw_log_binary ) + extended->total_params_size_bytes;
            return sizeof( extended_fw_log_binary ) + extended->total_params_size_bytes +
                                                      extended->number_of_params * sizeof( fw_logs::param_info );
        }

        command fw_logs_parser::get_start_command() const
        {
            command activate_command( 0 ); // Opcode would be overriden by the device
            activate_command.param1 = 1; // Update settings field - 1 for update settings
            // extended_log_request struct was designed to match the HWM command struct, copy remaining data
            activate_command.param2 = _verbosity_settings.module_filter[0];
            activate_command.param3 = _verbosity_settings.module_filter[1];
            activate_command.param4 = _verbosity_settings.module_filter[2];
            activate_command.data.assign( &_verbosity_settings.severity_level[0][0],
                                          &_verbosity_settings.severity_level[max_sources][0] );

            return activate_command;
        }

        command fw_logs_parser::get_update_command() const
        {
            command update_command( 0 );  // Opcode would be overriden by the device
            update_command.param1 = 0;  // Update settings field - 0 for no update needed (just getting logs)
            // All other fields are ignored

            return update_command;
        }

        command fw_logs_parser::get_stop_command() const
        {
            command stop_command( 0 );  // Opcode would be overriden by the device
            stop_command.param1 = 1;  // Update settings field - 1 for update settings
            // All other fields should remain 0 to clear settings

            return stop_command;
        }

        fw_logs_parser::structured_binary_data
        fw_logs_parser::structure_common_data( const fw_logs_binary_data * fw_log_msg ) const
        {
            structured_binary_data structured_data;

            auto * log_binary = reinterpret_cast< const fw_log_binary * >( fw_log_msg->logs_buffer.data() );

            structured_data.severity = static_cast< uint32_t >( log_binary->severity );
            structured_data.source_id = static_cast< uint32_t >( log_binary->source_id );
            structured_data.file_id = static_cast< uint32_t >( log_binary->file_id );
            structured_data.module_id = static_cast< uint32_t >( log_binary->module_id );
            structured_data.event_id = static_cast< uint32_t >( log_binary->event_id );
            structured_data.line = static_cast< uint32_t >( log_binary->line_id );
            structured_data.sequence = static_cast< uint32_t >( log_binary->seq_id );

            return structured_data;
        }

        void fw_logs_parser::structure_timestamp( const fw_logs_binary_data * raw,
                                                  structured_binary_data * structured ) const
        {
            const auto actual_struct = reinterpret_cast< const extended_fw_log_binary * >( raw->logs_buffer.data() );
            structured->timestamp = actual_struct->soc_timestamp;
        }

        void fw_logs_parser::structure_params( const fw_logs_binary_data * raw,
                                               size_t num_of_params,
                                               structured_binary_data * structured ) const
        {
            const auto actual_struct = reinterpret_cast< const extended_fw_log_binary * >( raw->logs_buffer.data() );
            if( num_of_params != actual_struct->number_of_params )
                throw librealsense::invalid_value_exception( rsutils::string::from() << "Expecting " << num_of_params
                                                             << " parameters, received " << actual_struct->number_of_params );

            if( num_of_params > 0 )
            {
                struct extended_fw_log_params_binary : public extended_fw_log_binary
                {
                    // param_info array size namber_of_params, 1 is just a placeholder. Need to use an array not a
                    // pointer because pointer can be 8 bytes of size and than we won't parse the data correctly
                    param_info info[1];
                };

                const auto with_params = reinterpret_cast< const extended_fw_log_params_binary * >( raw->logs_buffer.data() );
                const uint8_t * blob_start = reinterpret_cast< const uint8_t * >( with_params->info +
                                                                                  actual_struct->number_of_params );
                //size_t blob_size = actual_struct->total_params_size_bytes - num_of_params * sizeof( fw_logs::param_info );
                size_t blob_size = actual_struct->total_params_size_bytes;

                for( size_t i = 0; i < num_of_params; ++i )
                {
                    // Raw message offset is start of message, structured data offset is start of blob
                    size_t blob_offset = blob_start - raw->logs_buffer.data();
                    param_info adjusted_info = { static_cast< uint16_t >( with_params->info[i].offset - blob_offset ),
                                                 with_params->info[i].type,
                                                 with_params->info[i].size };
                    structured->params_info.push_back( adjusted_info );
                }
                structured->params_blob.assign( blob_start, blob_start + blob_size );
            }
        }

        const fw_logs_formatting_options & fw_logs_parser::get_format_options_ref( int source_id ) const
        {
            auto iter = _source_to_formatting_options.find( source_id );
            if( iter != _source_to_formatting_options.end() )
                return iter->second;

            throw librealsense::invalid_value_exception( rsutils::string::from()
                                                         << "Invalid source ID received " << source_id );
        }

        std::string fw_logs_parser::get_source_name( int source_id ) const
        {
            auto iter = _source_id_to_name.find( source_id );
            if( iter != _source_id_to_name.end() )
                return iter->second;

            throw librealsense::invalid_value_exception( rsutils::string::from()
                                                         << "Invalid source ID received " << source_id );
        }

        rs2_log_severity fw_logs_parser::parse_severity( uint32_t severity ) const
        {
            return fw_logs::fw_logs_severity_to_rs2_log_severity( severity );
        }

        legacy_fw_logs_parser::legacy_fw_logs_parser( const std::string & xml_content )
            : fw_logs_parser( xml_content )
        {
            // Check expected size here, no need to check repeatedly in parse functions
            if( _source_to_formatting_options.size() != 1 )
                throw librealsense::invalid_value_exception( rsutils::string::from()
                                                             << "Legacy parser expect one formating options, have "
                                                             << _source_to_formatting_options.size() );
        }

        size_t legacy_fw_logs_parser::get_log_size( const uint8_t * ) const
        {
            return sizeof( legacy_fw_log_binary );
        }

        void legacy_fw_logs_parser::structure_timestamp( const fw_logs_binary_data * raw,
                                                         structured_binary_data * structured ) const
        {
            const auto actual_struct = reinterpret_cast< const legacy_fw_log_binary * >( raw->logs_buffer.data() );
            structured->timestamp = actual_struct->timestamp;
        }

        void legacy_fw_logs_parser::structure_params( const fw_logs_binary_data * raw,
                                                      size_t num_of_params,
                                                      structured_binary_data * structured ) const
        {
            const auto actual_struct = reinterpret_cast< const legacy_fw_log_binary * >( raw->logs_buffer.data() );
            uint16_t params_size_bytes = 0;
            if( num_of_params > 0 )
            {
                structured->params_info.push_back( { params_size_bytes, param_type::UINT16, 2 } );  // P1, uint16_t
                params_size_bytes += 2;
            }
            if( num_of_params > 1 )
            {
                structured->params_info.push_back( { params_size_bytes, param_type::UINT16, 2 } );  // P2, uint16_t
                params_size_bytes += 2;
            }
            if( num_of_params > 2 )
            {
                structured->params_info.push_back( { params_size_bytes, param_type::UINT32, 4 } );  // P3, uint32_t
                params_size_bytes += 4;
            }
            if( num_of_params > 3 )
                throw librealsense::invalid_value_exception( rsutils::string::from() <<
                                                             "Expecting max 3 parameters, received " << num_of_params );

            const uint8_t * blob_start = reinterpret_cast< const uint8_t * >( &actual_struct->p1 );
            structured->params_blob.insert( structured->params_blob.end(), blob_start, blob_start + params_size_bytes );
        }

        const fw_logs_formatting_options & legacy_fw_logs_parser::get_format_options_ref( int source_id ) const
        {
            return _source_to_formatting_options.begin()->second;
        }

        std::string legacy_fw_logs_parser::get_source_name( int source_id ) const
        {
            // Legacy FW logs had threads, not source
            return _source_to_formatting_options.begin()->second.get_thread_name( source_id );
        }

        rs2_log_severity legacy_fw_logs_parser::parse_severity( uint32_t severity ) const
        {
            return fw_logs::legacy_fw_logs_severity_to_rs2_log_severity( severity );
        }
    }
}
