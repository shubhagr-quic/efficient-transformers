#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h> /* mmap() is defined in this header */
#include "QnnInterface.h"
#include "System/QnnSystemInterface.h"
const bool STREAM = false;
const int BIN_ARGV_OFFSET = 1;
const int INPUT_PROMPT_ARGV_OFFSET = 2;
const int NUM_CHUNKS_ARGV_OFFSET = 3;
const int INPUT_LEN_ARGV_OFFSET = 4; // ACTUAL INP LEN
const int CONTEXT_LEN_ARGV_OFFSET = 5;
const int GEN_LEN_ARGV_OFFSET = 6;
const int NUM_DEVICES_ARGV_OFFSET = 8;
const int EOS_TOKEN_ARGV_PARTIAL_OFFSET = 9; // TRUE_OFFSET = NUM_DEVICES + EOS_TOKEN_ARGV_PARTIAL_OFFSET

void exit_if_err(Qnn_ErrorHandle_t err, char *err_msg) {
  Qnn_ErrorHandle_t qnn_err_code = QNN_GET_ERROR_CODE(err);
  if (qnn_err_code != QNN_SUCCESS) {
    fprintf(stderr, "%s: %ld\n", err_msg, qnn_err_code);
    exit(qnn_err_code);
  }
}

typedef struct {
  /// app-accessible data pointer, provided by app.
  void* data;
  /// size of buffer, in bytes, pointed to by data.
  uint64_t dataSize;
} Qnn_ClientBuffer_t64;

Qnn_ClientBuffer_t64 load_file_64(const char *path) {
  struct stat stat;
  int sc, errno = 1;
  FILE *fd = fopen(path, "r");
  sc = fstat(fileno(fd), &stat);
  uint64_t size = stat.st_size;
  char *buffer = (char *) mmap(NULL, size, PROT_READ, MAP_PRIVATE, fileno(fd), 0);
  Qnn_ClientBuffer_t64 out = {
    .data = buffer,
    .dataSize = size,
  };
  fclose(fd);
  return out;
}

Qnn_ClientBuffer_t load_file(const char *path) {
  // fprintf(stdout, "loading file - %s\n", path);
  FILE *ctx_bin = fopen(path, "r");
  fseek(ctx_bin, 0, SEEK_END);
  uint64_t size = ftell(ctx_bin);
  fseek(ctx_bin, 0, SEEK_SET);
  char *data = malloc(size);
  fread(data, 1, size, ctx_bin);
  fclose(ctx_bin);
  Qnn_ClientBuffer_t out = {
      .data = data,
      .dataSize = size,
  };
  return out;
}

// Argmax of logits for each iteration
uint64_t argmax(float *logits, uint32_t vocab_size) {
  float logit_max = -INFINITY;
  uint64_t logit_argmax;
  for (int i = 0; i < vocab_size; i++)
    if (logits[i] > logit_max) {
      logit_max = logits[i];
      logit_argmax = i;
    }
  return logit_argmax;
}

// Argmax of logits for each iteration
void argmax_batchSize(float *logitsArr, uint32_t vocab_size,uint32_t batch_size, uint64_t *next_token ) {
  for(int j=0;j<batch_size;j++)
  {
    float logit_max = -INFINITY;
    uint64_t logit_argmax;
    float *logits = &logitsArr[j * vocab_size];
    for (int i = 0; i < vocab_size; i++)
      if (logits[i] > logit_max)
      {
        logit_max = logits[i];
        logit_argmax = i;
      }
    next_token[j] = logit_argmax;
  }
}

// Remove buffers from graph_info to avoid double free
void clean_up_graph_tensors(QnnSystemContext_GraphInfoV1_t ginfo) {
  for (int i = 0; i < ginfo.numGraphInputs; i++) {
    ginfo.graphInputs[i].v1.clientBuf.data = NULL;
  }
  for (int i = 0; i < ginfo.numGraphOutputs; i++) {
    ginfo.graphOutputs[i].v1.clientBuf.data = NULL;
  }
}

// Allocates memory and expands string array
char* string_concat(char* destination, char* source) {
  if (source == NULL){
    return destination; // if source is null, nothing to concat
  }
  char* destination_2 = (char*)malloc((strlen(destination) + strlen(source))*sizeof(char*));
  strcpy(destination_2, destination);
  strcat(destination_2, source);
  free(destination);
  return destination_2;
}
/* f"{run_kv} {qpc}
            \"{prompt_0}\"
            {num_chunks}
            {input_len}
            {ctx_len}
            {generation_len}
            {profiling_flag}
            {manual_devices}
            {tokenizer.eos_token_id}"
*/

int main(int argc, char *argv[]) {
  for(int i = 0; i < argc; i++)
  {
    printf("%s\n", argv[i]);
  }
  if (argc < 4) {
    fprintf(stderr, "Usage: %s <context_binary> [<prompt>]\n", argv[0]);
    return 1;
  }
  // Read tokens file
  Qnn_ClientBuffer_t tokens_buf = load_file("tokens.bin");
  uint32_t tokens_size = atoi(tokens_buf.data);
  char **tokens = calloc(tokens_size, sizeof(char *));
  char *curr = tokens_buf.data + strlen(tokens_buf.data) + 1;
  for (int i = 0; i < tokens_size; i++) {
    tokens[i] = curr;
    curr += strlen(curr) + 1;
  }
  fprintf(stdout,"RunKVCache Called");
  Qnn_ErrorHandle_t err;
  Qnn_ClientBuffer_t64 ctx_buf = load_file_64(argv[BIN_ARGV_OFFSET]);
  // Read graph info
  const QnnSystemInterface_t **providers;
  uint32_t num_providers;
  QnnSystemInterface_getProviders(&providers, &num_providers);
  if (num_providers == 0) {
    fprintf(stderr, "No providers found for system interface");
    return 1;
  }
  QNN_SYSTEM_INTERFACE_VER_TYPE qnn =
      providers[0]->QNN_SYSTEM_INTERFACE_VER_NAME;

  QnnSystemContext_Handle_t sysCtx;

  err = qnn.systemContextCreate(&sysCtx);
  exit_if_err(err, "Error creating system context");
  const QnnSystemContext_BinaryInfo_t *binfo;
  err = qnn.systemContextGetMetaData(sysCtx, ctx_buf.data, ctx_buf.dataSize,
                                     &binfo);
  exit_if_err(err, "Error getting binary info");
  QnnSystemContext_GraphInfoV1_t prefill_info;
  if(binfo->contextBinaryInfoV1.numGraphs == 1)
  {
    // decode only models with single graph
     prefill_info = binfo->contextBinaryInfoV1.graphs[0].graphInfoV1;
  }
  else
  {
     prefill_info = binfo->contextBinaryInfoV1.graphs[1].graphInfoV1;
  }
  QnnSystemContext_GraphInfoV1_t decode_info =
      binfo->contextBinaryInfoV1.graphs[0].graphInfoV1;
  // Read lengths
  uint32_t batch_size, prompt_len, input_len, ctx_len, vocab_size, gen_len;
  input_len = atoi(argv[INPUT_LEN_ARGV_OFFSET]);
  ctx_len = atoi(argv[CONTEXT_LEN_ARGV_OFFSET]);
  gen_len = atoi(argv[GEN_LEN_ARGV_OFFSET]);
  for (int i = 0; i < prefill_info.numGraphInputs; i++) {
    Qnn_TensorV1_t tensor = prefill_info.graphInputs[i].v1;
    if (strcmp(tensor.name, "input_ids") == 0) {
      batch_size = tensor.dimensions[0];
      prompt_len = tensor.dimensions[1];
    }
  }
  // [STORE TOKENS] : Step 1, allocate the out_str
	char* out_str[batch_size];
  for(int i=0; i <batch_size; i++)
  {
    out_str[i] = (char*)malloc(strlen(argv[INPUT_PROMPT_ARGV_OFFSET]) * sizeof(char*));
	  strcpy(out_str[i], argv[INPUT_PROMPT_ARGV_OFFSET]);
  }

  for (int i = 0; i < prefill_info.numGraphOutputs; i++) {
    Qnn_TensorV1_t tensor = prefill_info.graphOutputs[i].v1;
    if (strcmp(tensor.name, "logits") == 0) {
      uint32_t rank = tensor.rank;
      vocab_size = tensor.dimensions[rank - 1];
    }
  }
  fprintf(stdout, "batch_size %d prompt_len %d vocab_size %d ctx_len %d i Num of Graphs %d \n", batch_size, prompt_len, vocab_size, ctx_len, binfo->contextBinaryInfoV1.numGraphs);
  // Inference setup
  const QnnInterface_t **interfaces;
  uint32_t num_ifaces;
  uint32_t selected_iface;
  err = QnnInterface_getProviders(&interfaces, &num_ifaces);
  exit_if_err(err, "Error retrieving providers");
  for (selected_iface = 0; selected_iface < num_ifaces; selected_iface++) {
    if (strcmp(interfaces[selected_iface]->providerName, "AIC_QTI_AISW") == 0) {
      break;
    }
  }
  if (selected_iface == num_ifaces) {
    fprintf(stderr, "AIC backend not found among the interfaces:\n");
    for (int i = 0; i < num_ifaces; i++)
      fprintf(stdout,"%s\n", interfaces[i]->providerName);
  }
  QNN_INTERFACE_VER_TYPE aic =
      interfaces[selected_iface]->QNN_INTERFACE_VER_NAME;
  // Create logger
  Qnn_LogHandle_t log;
  err = aic.logCreate(NULL, QNN_LOG_LEVEL_ERROR, &log);
  exit_if_err(err, "Error creating logger");
  // Create backend
  Qnn_BackendHandle_t backend;
  err = aic.backendCreate(log, NULL, &backend);
  exit_if_err(err, "Error creating backend");
  // Need to use a single exec obj to guarantee correct ordering of inferences (prefill->decode)
  QnnContext_Config_t contextConfig = {
      .option             = QNN_CONTEXT_CONFIG_ASYNC_EXECUTION_QUEUE_DEPTH,
      .asyncExeQueueDepth = {.type  = QNN_CONTEXT_ASYNC_EXECUTION_QUEUE_DEPTH_TYPE_NUMERIC,
                             .depth = 1}};
  QnnContext_Config_t persistentConfig = { .option = QNN_CONTEXT_CONFIG_PERSISTENT_BINARY, .isPersistentBinary = true };
  const QnnContext_Config_t *contextConfigVec[] = {&contextConfig, &persistentConfig, NULL};
  uint8_t num_manual_devices= atoi(argv[NUM_DEVICES_ARGV_OFFSET]);
  // Create context based on availalble devices
  Qnn_ContextHandle_t context;
  Qnn_ProfileHandle_t profile_handle = NULL;

  if(num_manual_devices > 0 )
  {
    QnnDevice_HardwareDeviceInfo_t hardwareDeviceInfoVec[num_manual_devices];

    for(int i =0;i<num_manual_devices;i++)
    {
      hardwareDeviceInfoVec[i].version = QNN_DEVICE_HARDWARE_DEVICE_INFO_VERSION_1;
      hardwareDeviceInfoVec[i].v1.deviceId = atoi(argv[NUM_DEVICES_ARGV_OFFSET + (i + 1)]);
      hardwareDeviceInfoVec[i].v1.deviceType = 0; //unused
      hardwareDeviceInfoVec[i].v1.numCores = 0; //unused
      hardwareDeviceInfoVec[i].v1.cores = NULL; //unused
    }
    QnnDevice_PlatformInfo_t platformInfo ={.version = QNN_DEVICE_PLATFORM_INFO_VERSION_1,
                            .v1      = {.numHwDevices = num_manual_devices, .hwDevices = hardwareDeviceInfoVec}};

    QnnDevice_Config_t deviceConfig = {.option = QNN_DEVICE_CONFIG_OPTION_PLATFORM_INFO,
                                    .hardwareInfo = &platformInfo};
    const QnnDevice_Config_t * deviceConfigVec[] = {&deviceConfig, NULL};
    Qnn_DeviceHandle_t deviceHandle;
    err = aic.deviceCreate(log, deviceConfigVec, &deviceHandle);
    exit_if_err(err, "Failed to create device");

    err = aic.contextCreateFromBinary(backend, deviceHandle, contextConfigVec, ctx_buf.data,
                                      ctx_buf.dataSize, &context, profile_handle);
    exit_if_err(err, "Error creating backend");
  }
  else
  {
    // auto pick devices
    err = aic.contextCreateFromBinary(backend, NULL, contextConfigVec, ctx_buf.data,
                                      ctx_buf.dataSize, &context, profile_handle);
    exit_if_err(err, "Error creating Context");
  }
  // Retrieve graphs
  Qnn_GraphHandle_t prefill, decode;
  err = aic.graphRetrieve(context, prefill_info.graphName, &prefill);
  exit_if_err(err, "Error retrieving prefill graph");
  err = aic.graphRetrieve(context, decode_info.graphName, &decode);
  exit_if_err(err, "Error retrieving decode graph");
  uint64_t cache_index = 0;
  float *logits = calloc(batch_size * 1 * vocab_size, sizeof(float));
  // [CHUNKING] Loading inputs
  int num_chunks = atoi(argv[NUM_CHUNKS_ARGV_OFFSET]);
  Qnn_ClientBuffer_t* input_ids = (Qnn_ClientBuffer_t*)malloc(num_chunks * sizeof(Qnn_ClientBuffer_t));
  Qnn_ClientBuffer_t* position_ids = (Qnn_ClientBuffer_t*)malloc(num_chunks * sizeof(Qnn_ClientBuffer_t));
  for (int nc = 0; nc < num_chunks; nc++) {
    char nc_str[10];
    sprintf(nc_str, "%d", nc);
    char filepath[100];
    strcpy(filepath, "prefill/input_ids_");
    input_ids[nc] = load_file(strcat(strcat(filepath, nc_str), ".raw"));

    strcpy(filepath, "prefill/position_ids_");
    position_ids[nc] = load_file(strcat(strcat(filepath, nc_str), ".raw"));

  }
  // [PREFILL TIME] capture start time
  struct timespec prefill_start_time, prefill_end_time;
  QnnGraph_Config_t profiling_config = {
        .option = QNN_GRAPH_CONFIG_OPTION_PROFILE_HANDLE,
        .profileHandle = profile_handle };
  const QnnGraph_Config_t *graph_configs[] = {&profiling_config, NULL};
  // [CHUNKING] Decode + chunking loop
  for (int nc = 0; nc < num_chunks; nc++) {
    for (int i = 0; i < prefill_info.numGraphInputs; i++) {
      const char *tensor_name = prefill_info.graphInputs[i].v1.name;
      if (strcmp(tensor_name, "input_ids") == 0) {
        prefill_info.graphInputs[i].v1.clientBuf = input_ids[nc];
      } else if (strcmp(tensor_name, "position_ids") == 0) {
        prefill_info.graphInputs[i].v1.clientBuf = position_ids[nc];
      }
    }
    // Set outputs
    for (int i = 0; i < prefill_info.numGraphOutputs; i++) {
      const char *tensor_name = prefill_info.graphOutputs[i].v1.name;
      if (strcmp(tensor_name, "logits") == 0) {
        prefill_info.graphOutputs[i].v1.clientBuf.data = logits;
        prefill_info.graphOutputs[i].v1.clientBuf.dataSize =
            batch_size * 1 * vocab_size * sizeof(float);
      }
    }
    err = aic.graphExecute(prefill, prefill_info.graphInputs,
                          prefill_info.numGraphInputs, prefill_info.graphOutputs,
                          prefill_info.numGraphOutputs, NULL, NULL);
    exit_if_err(err, "Error executing prefill graph");

    cache_index += prompt_len;
  }
  // [PREFILL TIME] get end time
  uint64_t next_token[batch_size];
  argmax_batchSize(logits, vocab_size,batch_size,next_token);  // for batchsize

  uint64_t *next_token_ptr = next_token;

  for(int i=0; i < batch_size; i++)
  {
    // [STORE TOKENS] : Step 2, concat the token after prefill
    out_str[i] = string_concat(out_str[i], tokens[next_token[i]]);
    if(STREAM == true){
       fprintf(stdout, " next token %s", tokens[next_token[i]]);
    }
   }
  // Prepare for next iteration, calculating decode pos id
  uint64_t decode_pos_id[batch_size];
  for(int j = 0; j < batch_size; j++)
  {
   decode_pos_id[j] = (uint64_t) input_len;
   fprintf(stderr, "decode_pos_id - %lu for batch - %d:\n", decode_pos_id[j], j);
  }
  // Clean-up prefill tensors
  free(input_ids);
  free(position_ids);
  clean_up_graph_tensors(prefill_info);

  // Set inputs for decode stage
  for (int i = 0; i < decode_info.numGraphInputs; i++) {
    const char *tensor_name = prefill_info.graphInputs[i].v1.name;
    if (strcmp(tensor_name, "input_ids") == 0) {
      decode_info.graphInputs[i].v1.clientBuf.data = next_token_ptr;
      decode_info.graphInputs[i].v1.clientBuf.dataSize = sizeof(uint64_t)*batch_size;
    } else if (strcmp(tensor_name, "position_ids") == 0) {
      decode_info.graphInputs[i].v1.clientBuf.data = decode_pos_id;
      decode_info.graphInputs[i].v1.clientBuf.dataSize = sizeof(uint64_t)*batch_size;
    }
  }
  // Set outputs for decode stage
  for (int i = 0; i < decode_info.numGraphOutputs; i++) {
    const char *tensor_name = decode_info.graphOutputs[i].v1.name;
    if (strcmp(tensor_name, "logits") == 0) {
      decode_info.graphOutputs[i].v1.clientBuf.data = logits;
      decode_info.graphOutputs[i].v1.clientBuf.dataSize =
          batch_size * 1 * vocab_size * sizeof(float);
    }
  }

  bool end_of_sentence = false;
  // Execute decode in loop
  for (int i = 0; i < gen_len - 1; i++) {
    if(end_of_sentence)
      break;
    err = aic.graphExecute(decode, decode_info.graphInputs,
                           decode_info.numGraphInputs, decode_info.graphOutputs,
                           decode_info.numGraphOutputs, NULL, NULL);
    exit_if_err(err, "Error executing decode graph");
    uint64_t next_token_decode[batch_size];
    argmax_batchSize(logits, vocab_size,batch_size,next_token_ptr);  // for batchsize
    cache_index += 1;
    for(int j=0; j < batch_size; j++)
    {
      decode_pos_id[j] += 1;

      // [STORE TOKENS] : Step 2, concat the token after prefill
      out_str[j] = string_concat(out_str[j], tokens[next_token[j]]);

      if(STREAM == true){
        fprintf(stdout,"%s", tokens[next_token[j]]);
        fflush(stdout);
      }
      // if optional EOS token is provided
      if (argc > (num_manual_devices + EOS_TOKEN_ARGV_PARTIAL_OFFSET - 1)) {
	      uint64_t end_token = atoi(argv[num_manual_devices + EOS_TOKEN_ARGV_PARTIAL_OFFSET]);
        end_of_sentence = next_token[j] == end_token;
      }
    }
  }
  // [RESULTS]
  // double prefill_perf = 1 / (diff_timespec(&prefill_end_time, &prefill_start_time));
  // double decode_perf = (cache_index - prompt_len * num_chunks - 1 ) / diff_timespec(&decode_end_time,&decode_start_time);
  // double total_perf = (cache_index - prompt_len * num_chunks ) / diff_timespec(&decode_end_time, &prefill_start_time);
  // fprintf(stderr, "decode end - prefill start = %f\n",  diff_timespec(&decode_end_time, &prefill_start_time));
  // fprintf(stderr, "decode time + prefill time = %f\n",  (
  //   diff_timespec(&prefill_end_time, &prefill_start_time) +
  //   diff_timespec(&decode_end_time,&decode_start_time)
  // ));
  // cleanup
  clean_up_graph_tensors(decode_info);
  exit_if_err(qnn.systemContextFree(sysCtx), "Error freeing system context");
  exit_if_err(aic.contextFree(context, profile_handle), "Error freeing context");
  exit_if_err(aic.backendFree(backend), "Error freeing backend");
  exit_if_err(aic.logFree(log), "Error freeing logger");
  // [STORE TOKENS] : Step 3, print all the tokens
  fprintf(stdout, "[INPUT] %s\n", argv[INPUT_PROMPT_ARGV_OFFSET]);
  for(int i =0;i<batch_size;i++)
  {
    fprintf(stdout, "[OUTPUT] %s\n", out_str[i]);
  }
  // fprintf(stdout, "[RESULT] { \"prefill_tps\" : %f, \"decode_tps\" : %f, \"total_tps\" : %f }\n",
  // prefill_perf, decode_perf, total_perf);
  // Free up allocated memory
  free(logits);
  free(tokens);
  free(tokens_buf.data);
  munmap(ctx_buf.data, ctx_buf.dataSize);
  for(int i=0;i <batch_size;i++)
  {
    free(out_str[i]);
  }
  fprintf(stdout, "End of KV cache c file\n");
  return 0;
}