/*
 * Copyright (C) 2011 Sansar Choinyambuu
 * HSR Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "imc_attestation_state.h"

#include <imc/imc_agent.h>
#include <pa_tnc/pa_tnc_msg.h>
#include <ietf/ietf_attr.h>
#include <ietf/ietf_attr_pa_tnc_error.h>

#include <tcg/tcg_pts_attr_proto_caps.h>
#include <tcg/tcg_pts_attr_meas_algo.h>
#include <tcg/tcg_pts_attr_get_tpm_version_info.h>
#include <tcg/tcg_pts_attr_tpm_version_info.h>
#include <tcg/tcg_pts_attr_get_aik.h>
#include <tcg/tcg_pts_attr_aik.h>
#include <tcg/tcg_pts_attr_req_funct_comp_evid.h>
#include <tcg/tcg_pts_attr_gen_attest_evid.h>
#include <tcg/tcg_pts_attr_simple_comp_evid.h>
#include <tcg/tcg_pts_attr_simple_evid_final.h>
#include <tcg/tcg_pts_attr_req_file_meas.h>
#include <tcg/tcg_pts_attr_file_meas.h>

#include <tncif_pa_subtypes.h>

#include <pen/pen.h>
#include <debug.h>
#include <utils/linked_list.h>
#include <crypto/hashers/hasher.h>
#include <dirent.h>
#include <errno.h>

/* IMC definitions */

static const char imc_name[] = "Attestation";

#define IMC_VENDOR_ID				PEN_TCG
#define IMC_SUBTYPE					PA_SUBTYPE_TCG_PTS
#define IMC_ATTESTATION_BUF_SIZE	32768

static imc_agent_t *imc_attestation;

/**
 * Supported PTS measurement algorithms
 */
static pts_meas_algorithms_t supported_algorithms = 0;

/**
 * List of files and directories to measure
 */
static linked_list_t *file_list, *directory_list;

/**
 * List of file measurements
 */
static linked_list_t *file_measurements;

/* TODO: Move the struct to some header file? Duplicate with imv_attestation*/
/**
 * Struct to hold file or directory name with the request ID for Request File Measurement attribute
 */
typedef struct measurement_req_entry_t measurement_req_entry_t;

struct measurement_req_entry_t {
	char *path;
	u_int16_t request_id;
};
 
/**
 * see section 3.7.1 of TCG TNC IF-IMC Specification 1.2
 */
TNC_Result TNC_IMC_Initialize(TNC_IMCID imc_id,
							  TNC_Version min_version,
							  TNC_Version max_version,
							  TNC_Version *actual_version)
{
	if (imc_attestation)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has already been initialized", imc_name);
		return TNC_RESULT_ALREADY_INITIALIZED;
	}
	imc_attestation = imc_agent_create(imc_name, IMC_VENDOR_ID, IMC_SUBTYPE,
								imc_id, actual_version);
	if (!imc_attestation || !pts_meas_probe_algorithms(&supported_algorithms))
	{
		return TNC_RESULT_FATAL;
	}
	if (min_version > TNC_IFIMC_VERSION_1 || max_version < TNC_IFIMC_VERSION_1)
	{
		DBG1(DBG_IMC, "no common IF-IMC version");
		return TNC_RESULT_NO_COMMON_VERSION;
	}
	return TNC_RESULT_SUCCESS;
}

/**
 * see section 3.7.2 of TCG TNC IF-IMC Specification 1.2
 */
TNC_Result TNC_IMC_NotifyConnectionChange(TNC_IMCID imc_id,
										  TNC_ConnectionID connection_id,
										  TNC_ConnectionState new_state)
{
	imc_state_t *state;
	/* TODO: Not used so far */
	//imc_attestation_state_t *attestation_state;

	if (!imc_attestation)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}
	switch (new_state)
	{
		case TNC_CONNECTION_STATE_CREATE:
			state = imc_attestation_state_create(connection_id);
			return imc_attestation->create_state(imc_attestation, state);
		case TNC_CONNECTION_STATE_DELETE:
			return imc_attestation->delete_state(imc_attestation, connection_id);
		case TNC_CONNECTION_STATE_HANDSHAKE:
		case TNC_CONNECTION_STATE_ACCESS_ISOLATED:
		case TNC_CONNECTION_STATE_ACCESS_NONE:
		default:
			return imc_attestation->change_state(imc_attestation, connection_id,
												  new_state, NULL);
	}
}


/**
 * Get Hash Measurement of a file
 */
static TNC_Result hash_file(char *path, char *out)
{
	char buffer[IMC_ATTESTATION_BUF_SIZE];
	FILE *file;
	int bytes_read;
	pts_meas_algorithms_t selected_algorithm;
	hasher_t *hasher;
	hash_algorithm_t hash_alg;
	
	/* Create a hasher */
	selected_algorithm = PTS_MEAS_ALGO_SHA256; /* temporary fix, move to pts */
	hash_alg = pts_meas_to_hash_algorithm(selected_algorithm);
	hasher = lib->crypto->create_hasher(lib->crypto, hash_alg);
	if (!hasher)
	{
		DBG1(DBG_IMC, "hasher %N not available", hash_algorithm_names, hash_alg);
		return TNC_RESULT_FATAL;
	}

	file = fopen(path, "rb");
	if (!file)
	{
		DBG1(DBG_IMC,"file '%s' can not be opened", path);
		hasher->destroy(hasher);
		return TNC_RESULT_FATAL;
	}
	while (TRUE)
	{
		bytes_read = fread(buffer, 1, sizeof(buffer), file);
		if (bytes_read > 0)
		{
			hasher->get_hash(hasher, chunk_create(buffer, bytes_read), NULL);
		}
		else
		{
			hasher->get_hash(hasher, chunk_empty, out);
			break;
		}
	}
	fclose(file);
	hasher->destroy(hasher);

	return TNC_RESULT_SUCCESS;
}

/**
 * Get hash of all the files in a directory
 */
static TNC_Result hash_directory(char *path)
{
	DIR *dir;
	struct dirent *ent;
	linked_list_t *file_measurements;
	file_meas_entry_t *entry;
	
	file_measurements = linked_list_create();
	entry = malloc_thing(file_meas_entry_t);
	
	dir = opendir(path);
	if (dir == NULL)
	{
		DBG1(DBG_IMC, "opening directory '%s' failed: %s", path, strerror(errno));
		return TNC_RESULT_FATAL;
	}
	while ((ent = readdir(dir)))
	{
		char *file_hash;
		
		if(hash_file(ent->d_name,file_hash) != TNC_RESULT_SUCCESS)
		{
			DBG1(DBG_IMC, "Hashing the given file has failed");
			return TNC_RESULT_FATAL;
		}
		
		entry->measurement = chunk_create(file_hash,strlen(file_hash));
		entry->file_name_len = strlen(ent->d_name);
		entry->file_name = chunk_create(ent->d_name,strlen(ent->d_name));
		
		file_measurements->insert_last(file_measurements,entry);
	}
	closedir(dir);
	
	return TNC_RESULT_SUCCESS;
}

static TNC_Result send_message(TNC_ConnectionID connection_id)
{
	pa_tnc_msg_t *msg;
	pa_tnc_attr_t *attr;
	pts_t *pts;
	imc_state_t *state;
	imc_attestation_state_t *attestation_state;
	imc_attestation_handshake_state_t handshake_state;
	TNC_Result result;

	if (!imc_attestation->get_state(imc_attestation, connection_id, &state))
	{
		return TNC_RESULT_FATAL;
	}
	attestation_state = (imc_attestation_state_t*)state;
	handshake_state = attestation_state->get_handshake_state(attestation_state);
	pts = attestation_state->get_pts(attestation_state);
	
	/* Switch on the attribute type IMC has received */
	switch (handshake_state)
	{
		case IMC_ATTESTATION_STATE_REQ_PROTO_CAPS:
		{
			pts_proto_caps_flag_t flags;

			flags = pts->get_proto_caps(pts);
			attr = tcg_pts_attr_proto_caps_create(flags, FALSE);
			break;
		}
		case IMC_ATTESTATION_STATE_REQ_MEAS_ALGO:
		{
			pts_meas_algorithms_t selected_algorithm;

			selected_algorithm = pts->get_meas_algorithm(pts);
			attr = tcg_pts_attr_meas_algo_create(selected_algorithm, TRUE);
			break;
		}
		case IMC_ATTESTATION_STATE_GET_TPM_INFO:
		{
			chunk_t tpm_version_info;

			if (!pts->get_tpm_version_info(pts, &tpm_version_info))
			{
				/* TODO return TCG_PTS_TPM_VERS_NOT_SUPPORTED error attribute */
			}
			attr = tcg_pts_attr_tpm_version_info_create(tpm_version_info);
			break;
		}
		case IMC_ATTESTATION_STATE_REQ_FILE_MEAS:
		{
			measurement_req_entry_t *entry;
			enumerator_t *enumerator;
			tcg_pts_attr_file_meas_t *attr_file_meas;
			u_int16_t meas_len = HASH_SIZE_SHA1;
			pts_meas_algorithms_t selected_algorithm;
			
			selected_algorithm = PTS_MEAS_ALGO_SHA256; /* temporary fix, move to pts */
			if (selected_algorithm & PTS_MEAS_ALGO_SHA384)
			{
				meas_len = HASH_SIZE_SHA384;
			}
			else if(selected_algorithm & PTS_MEAS_ALGO_SHA256) 
			{
				meas_len = HASH_SIZE_SHA512;
			}

			msg = pa_tnc_msg_create();
			
			/** 
			 * Hash the files and add them as attribute
			 */
			enumerator = enumerator_create_single(file_list, NULL);
			while (enumerator->enumerate(enumerator, &entry))
			{
				char * file_hash;
				
				attr = tcg_pts_attr_file_meas_create(1, 
						entry->request_id, meas_len);
				attr->set_noskip_flag(attr, TRUE);
				attr_file_meas = (tcg_pts_attr_file_meas_t*)attr;
				
				if(hash_file(entry->path,file_hash) != TNC_RESULT_SUCCESS)
				{
					DBG1(DBG_IMC, "Hashing the given file has failed");
					return TNC_RESULT_FATAL;
				}
				attr_file_meas->add_file_meas(attr_file_meas, 
						chunk_create(file_hash,strlen(file_hash)),
						chunk_create(entry->path,strlen(entry->path)));
				
				msg->add_attribute(msg, attr);
			}
			
			/** 
			 * Hash the files in each directory and add them as attribute
			 */
			enumerator = enumerator_create_single(directory_list, NULL);
			while (enumerator->enumerate(enumerator, &entry))
			{
				enumerator_t *meas_enumerator;
				file_meas_entry_t *meas_entry;
				u_int64_t num_of_files = 0 ;
				
				if(hash_directory(entry->path) != TNC_RESULT_SUCCESS)
				{
					DBG1(DBG_IMC, "Hashing the files in a given directory has failed");
					return TNC_RESULT_FATAL;
				}
				
				attr = tcg_pts_attr_file_meas_create(0, 
						entry->request_id, meas_len);
				attr->set_noskip_flag(attr, TRUE);
				attr_file_meas = (tcg_pts_attr_file_meas_t*)attr;
				
				meas_enumerator = enumerator_create_single(file_measurements, NULL);
				while (meas_enumerator->enumerate(meas_enumerator, &meas_entry))
				{
					num_of_files++;
					attr_file_meas->add_file_meas(attr_file_meas,
								      meas_entry->measurement,
								      meas_entry->file_name);
				}
				
				attr_file_meas->set_number_of_files(attr_file_meas,
								    num_of_files);
				msg->add_attribute(msg, attr);
			}
			enumerator->destroy(enumerator);
			goto end;
		}
		case IMC_ATTESTATION_STATE_GET_AIK:
			/* TODO: Implement AIK retrieve */
		case IMC_ATTESTATION_STATE_REQ_FUNCT_COMP_EVID:
		case IMC_ATTESTATION_STATE_GEN_ATTEST_EVID:
		case IMC_ATTESTATION_STATE_REQ_FILE_METADATA:
		case IMC_ATTESTATION_STATE_REQ_IML:
		case IMC_ATTESTATION_STATE_INIT:
			DBG1(DBG_IMC, "Attestation IMC has nothing to send: \"%s\"", handshake_state);
			return TNC_RESULT_FATAL;
		default:
			DBG1(DBG_IMC, "Attestation IMC is in unknown state: \"%s\"", handshake_state);
			return TNC_RESULT_FATAL;
	}
	
	
	attr->set_noskip_flag(attr, TRUE);
	msg = pa_tnc_msg_create();
	msg->add_attribute(msg, attr);
	
end:
	msg->build(msg);
	result = imc_attestation->send_message(imc_attestation, connection_id,
									msg->get_encoding(msg));	
	msg->destroy(msg);

	return result;
}

/**
 * see section 3.7.3 of TCG TNC IF-IMC Specification 1.2
 */
TNC_Result TNC_IMC_BeginHandshake(TNC_IMCID imc_id,
								  TNC_ConnectionID connection_id)
{
	if (!imc_attestation)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}
	return TNC_RESULT_SUCCESS;
}

/**
 * see section 3.7.4 of TCG TNC IF-IMC Specification 1.2
 */
TNC_Result TNC_IMC_ReceiveMessage(TNC_IMCID imc_id,
								  TNC_ConnectionID connection_id,
								  TNC_BufferReference msg,
								  TNC_UInt32 msg_len,
								  TNC_MessageType msg_type)
{
	pa_tnc_msg_t *pa_tnc_msg;
	pa_tnc_attr_t *attr;
	imc_state_t *state;
	imc_attestation_state_t *attestation_state;
	enumerator_t *enumerator;
	pts_t *pts;
	TNC_Result result;
	bool fatal_error = FALSE;

	if (!imc_attestation)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}

	/* get current IMC state */
	if (!imc_attestation->get_state(imc_attestation, connection_id, &state))
	{
		return TNC_RESULT_FATAL;
	}
	attestation_state = (imc_attestation_state_t*)state;
	pts = attestation_state->get_pts(attestation_state);

	/* parse received PA-TNC message and automatically handle any errors */ 
	result = imc_attestation->receive_message(imc_attestation, connection_id,
									   chunk_create(msg, msg_len), msg_type,
									   &pa_tnc_msg);

	/* no parsed PA-TNC attributes available if an error occurred */
	if (!pa_tnc_msg)
	{
		return result;
	}

	/* analyze PA-TNC attributes */
	enumerator = pa_tnc_msg->create_attribute_enumerator(pa_tnc_msg);
	while (enumerator->enumerate(enumerator, &attr))
	{
		if (attr->get_vendor_id(attr) == PEN_IETF &&
			attr->get_type(attr) == IETF_ATTR_PA_TNC_ERROR)
		{
			ietf_attr_pa_tnc_error_t *error_attr;
			pa_tnc_error_code_t error_code;
			chunk_t msg_info, attr_info;
			u_int32_t offset;

			error_attr = (ietf_attr_pa_tnc_error_t*)attr;
			error_code = error_attr->get_error_code(error_attr);
			msg_info = error_attr->get_msg_info(error_attr);

			DBG1(DBG_IMC, "received PA-TNC error '%N' concerning message %#B",
				 pa_tnc_error_code_names, error_code, &msg_info);
			switch (error_code)
			{
				case PA_ERROR_INVALID_PARAMETER:
					offset = error_attr->get_offset(error_attr);
					DBG1(DBG_IMC, "  occurred at offset of %u bytes", offset);
					break;
				case PA_ERROR_ATTR_TYPE_NOT_SUPPORTED:
					attr_info = error_attr->get_attr_info(error_attr);
					DBG1(DBG_IMC, "  unsupported attribute %#B", &attr_info);
					break;
				default:
					break;
			}
			fatal_error = TRUE;
		}
		else if (attr->get_vendor_id(attr) == PEN_TCG)
		{
			switch(attr->get_type(attr))
			{
				case TCG_PTS_REQ_PROTO_CAPS:
				{
					tcg_pts_attr_proto_caps_t *attr_cast;
					pts_proto_caps_flag_t imc_flags, imv_flags;

					attr_cast = (tcg_pts_attr_proto_caps_t*)attr;
					imv_flags = attr_cast->get_flags(attr_cast);
					imc_flags = pts->get_proto_caps(pts);
					pts->set_proto_caps(pts, imc_flags & imv_flags);				
					
					attestation_state->set_handshake_state(attestation_state,
										IMC_ATTESTATION_STATE_REQ_PROTO_CAPS);
					break;
				}
				case TCG_PTS_MEAS_ALGO:
				{
					tcg_pts_attr_meas_algo_t *attr_cast;
					pts_meas_algorithms_t selected_algorithm;
					
					attr_cast = (tcg_pts_attr_meas_algo_t*)attr;
					selected_algorithm = attr_cast->get_algorithms(attr_cast);

					if ((supported_algorithms & PTS_MEAS_ALGO_SHA384) &&
						(selected_algorithm   & PTS_MEAS_ALGO_SHA384))
					{
						selected_algorithm = PTS_MEAS_ALGO_SHA384;
					}
					else if (selected_algorithm & PTS_MEAS_ALGO_SHA256)
					{
						selected_algorithm = PTS_MEAS_ALGO_SHA256;
					}
					else if (selected_algorithm & PTS_MEAS_ALGO_SHA1)
					{
						selected_algorithm = PTS_MEAS_ALGO_SHA1;
					}
					else
					{
						/* TODO send a TCG_PTS_HASH_ALG_NOT_SUPPORTED error */
					}
					pts->set_meas_algorithm(pts, selected_algorithm);

					attestation_state->set_handshake_state(attestation_state,
										IMC_ATTESTATION_STATE_REQ_MEAS_ALGO);
					break;
				}
					
				case TCG_PTS_GET_TPM_VERSION_INFO:
				{
					attestation_state->set_handshake_state(attestation_state,
										IMC_ATTESTATION_STATE_GET_TPM_INFO);
					break;
				}
				case TCG_PTS_GET_AIK:
				{
					attestation_state->set_handshake_state(attestation_state,
										IMC_ATTESTATION_STATE_GET_AIK);
					break;
				}
	
				/* PTS-based Attestation Evidence */
				case TCG_PTS_REQ_FUNCT_COMP_EVID:
					break;
				case TCG_PTS_GEN_ATTEST_EVID:
					break;
				case TCG_PTS_REQ_FILE_MEAS:
				{
					tcg_pts_attr_req_file_meas_t *attr_cast;
					measurement_req_entry_t *entry;
					u_int32_t delimiter;
					
					attr_cast = (tcg_pts_attr_req_file_meas_t*)attr;
					file_list = linked_list_create();
					directory_list = linked_list_create();
					delimiter = attr_cast->get_delimiter(attr_cast);
					entry = malloc_thing(measurement_req_entry_t);
					entry->request_id = attr_cast->get_request_id(attr_cast);
					entry->path = attr_cast->get_file_path(attr_cast).ptr;
					
					attr_cast->get_directory_flag(attr_cast) ? 
						directory_list->insert_last(directory_list, entry) : 
						file_list->insert_last(file_list, entry); 
					
					attestation_state->set_handshake_state(attestation_state,
										IMC_ATTESTATION_STATE_REQ_FILE_MEAS);
					break;
				}
				
				/* TODO: Not implemented yet */
				case TCG_PTS_DH_NONCE_PARAMS_REQ:
				case TCG_PTS_DH_NONCE_FINISH:
				case TCG_PTS_REQ_FILE_META:
				case TCG_PTS_REQ_INTEG_MEAS_LOG:
				/* Attributes using XML */
				case TCG_PTS_REQ_TEMPL_REF_MANI_SET_META:
				case TCG_PTS_UPDATE_TEMPL_REF_MANI:
				/* On Windows only*/
				case TCG_PTS_REQ_REGISTRY_VALUE:
				/* Received on IMV side only*/
				case TCG_PTS_PROTO_CAPS:
				case TCG_PTS_DH_NONCE_PARAMS_RESP:
				case TCG_PTS_MEAS_ALGO_SELECTION:
				case TCG_PTS_TPM_VERSION_INFO:
				case TCG_PTS_TEMPL_REF_MANI_SET_META:
				case TCG_PTS_AIK:				
				case TCG_PTS_SIMPLE_COMP_EVID:
				case TCG_PTS_SIMPLE_EVID_FINAL:
				case TCG_PTS_VERIFICATION_RESULT:
				case TCG_PTS_INTEG_REPORT:
				case TCG_PTS_UNIX_FILE_META:
				case TCG_PTS_FILE_MEAS:
				case TCG_PTS_INTEG_MEAS_LOG:
				default:
					DBG1(DBG_IMC, "received unsupported attribute '%N'",
						tcg_attr_names, attr->get_type(attr));
					break;
			}
			
			
		}
	}
	enumerator->destroy(enumerator);
	pa_tnc_msg->destroy(pa_tnc_msg);

	/* if no error occurred then always return the same response */
	return fatal_error ? TNC_RESULT_FATAL : send_message(connection_id);
}

/**
 * see section 3.7.5 of TCG TNC IF-IMC Specification 1.2
 */
TNC_Result TNC_IMC_BatchEnding(TNC_IMCID imc_id,
							   TNC_ConnectionID connection_id)
{
	if (!imc_attestation)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}
	return TNC_RESULT_SUCCESS;
}

/**
 * see section 3.7.6 of TCG TNC IF-IMC Specification 1.2
 */
TNC_Result TNC_IMC_Terminate(TNC_IMCID imc_id)
{
	if (!imc_attestation)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}
	imc_attestation->destroy(imc_attestation);
	imc_attestation = NULL;

	return TNC_RESULT_SUCCESS;
}

/**
 * see section 4.2.8.1 of TCG TNC IF-IMC Specification 1.2
 */
TNC_Result TNC_IMC_ProvideBindFunction(TNC_IMCID imc_id,
									   TNC_TNCC_BindFunctionPointer bind_function)
{
	if (!imc_attestation)
	{
		DBG1(DBG_IMC, "IMC \"%s\" has not been initialized", imc_name);
		return TNC_RESULT_NOT_INITIALIZED;
	}
	return imc_attestation->bind_functions(imc_attestation, bind_function);
}
