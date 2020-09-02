
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <eigen3/Eigen/Dense>

#include "common/timing.h"
#include "common/params.h"
#include "driving.h"

#define TRAJECTORY_SIZE 33
#define TRAJECTORY_TIME 10.0
#define PLAN_IDX 0
#define LL_IDX PLAN_IDX + PLAN_MHP_N*PLAN_MHP_GROUP_SIZE
#define LL_PROB_IDX LL_IDX + 4*2*2*33
#define RE_IDX LL_PROB_IDX + 4
#define LEAD_IDX RE_IDX + 2*2*2*33
#define LEAD_PROB_IDX LEAD_MHP_N*LEAD_MHP_GROUP_SIZE
#define DESIRE_STATE_IDX LEAD_PROB_IDX + 3
#define META_IDX DESIRE_STATE_IDX + DESIRE_LEN
#define POSE_IDX META_IDX + OTHER_META_SIZE + DESIRE_PRED_SIZE
#define OUTPUT_SIZE  POSE_IDX + POSE_SIZE
#ifdef TEMPORAL
  #define TEMPORAL_SIZE 512
#else
  #define TEMPORAL_SIZE 0
#endif

// #define DUMP_YUV

Eigen::Matrix<float, MODEL_PATH_DISTANCE, POLYFIT_DEGREE - 1> vander;
float X_IDXS[TRAJECTORY_SIZE];
float T_IDXS[TRAJECTORY_SIZE];

void model_init(ModelState* s, cl_device_id device_id, cl_context context, int temporal) {
  frame_init(&s->frame, MODEL_WIDTH, MODEL_HEIGHT, device_id, context);
  s->input_frames = (float*)calloc(MODEL_FRAME_SIZE * 2, sizeof(float));

  const int output_size = OUTPUT_SIZE + TEMPORAL_SIZE;
  s->output = (float*)calloc(output_size, sizeof(float));

  s->m = new DefaultRunModel("../../models/supercombo.dlc", s->output, output_size, USE_GPU_RUNTIME);

#ifdef TEMPORAL
  assert(temporal);
  s->m->addRecurrent(&s->output[OUTPUT_SIZE], TEMPORAL_SIZE);
#endif

#ifdef DESIRE
  s->prev_desire = std::make_unique<float[]>(DESIRE_LEN);
  s->pulse_desire = std::make_unique<float[]>(DESIRE_LEN);
  s->m->addDesire(s->pulse_desire.get(), DESIRE_LEN);
#endif

#ifdef TRAFFIC_CONVENTION
  s->traffic_convention = std::make_unique<float[]>(TRAFFIC_CONVENTION_LEN);
  s->m->addTrafficConvention(s->traffic_convention.get(), TRAFFIC_CONVENTION_LEN);

  bool is_rhd = Params().read_db_bool("IsRHD");
  if (is_rhd) {
    s->traffic_convention[1] = 1.0;
  } else {
    s->traffic_convention[0] = 1.0;
  }
#endif

  // Build Vandermonde matrix
  for(int i = 0; i < TRAJECTORY_SIZE; i++) {
    for(int j = 0; j < POLYFIT_DEGREE - 1; j++) {
      X_IDXS[i] = (192.0/1024.0) * (i**2)
      T_IDXS[i] = (10.0/1024.0) * (i**2)
      vander(i, j) = pow(X_IDXS[i], POLYFIT_DEGREE-j-1);
    }
  }
}

ModelDataRaw model_eval_frame(ModelState* s, cl_command_queue q,
                           cl_mem yuv_cl, int width, int height,
                           mat3 transform, void* sock,
                           float *desire_in) {
#ifdef DESIRE
  if (desire_in != NULL) {
    for (int i = 0; i < DESIRE_LEN; i++) {
      // Model decides when action is completed
      // so desire input is just a pulse triggered on rising edge
      if (desire_in[i] - s->prev_desire[i] > .99) {
        s->pulse_desire[i] = desire_in[i];
      } else {
        s->pulse_desire[i] = 0.0;
      }
      s->prev_desire[i] = desire_in[i];
    }
  }
#endif

  //for (int i = 0; i < OUTPUT_SIZE + TEMPORAL_SIZE; i++) { printf("%f ", s->output[i]); } printf("\n");

  float *new_frame_buf = frame_prepare(&s->frame, q, yuv_cl, width, height, transform);
  memmove(&s->input_frames[0], &s->input_frames[MODEL_FRAME_SIZE], sizeof(float)*MODEL_FRAME_SIZE);
  memmove(&s->input_frames[MODEL_FRAME_SIZE], new_frame_buf, sizeof(float)*MODEL_FRAME_SIZE);
  s->m->execute(s->input_frames, MODEL_FRAME_SIZE*2);

  #ifdef DUMP_YUV
    FILE *dump_yuv_file = fopen("/sdcard/dump.yuv", "wb");
    fwrite(new_frame_buf, MODEL_HEIGHT*MODEL_WIDTH*3/2, sizeof(float), dump_yuv_file);
    fclose(dump_yuv_file);
    assert(1==2);
  #endif

  clEnqueueUnmapMemObject(q, s->frame.net_input, (void*)new_frame_buf, 0, NULL, NULL);

  // net outputs
  ModelDataRaw net_outputs;
  net_outputs.path = &s->output[PATH_IDX];
  net_outputs.lane_lines = &s->output[LL_IDX];
  net_outputs.lane_lines_prob = &s->output[LL_PROB_IDX];
  net_outputs.road_edges = &s->output[RE_IDX];
  net_outputs.lead = &s->output[LEAD_IDX];
  net_outputs.lead_prob = &s->output[LEAD_PROB_IDX];
  net_outputs.meta = &s->output[DESIRE_STATE_IDX];
  net_outputs.pose = &s->output[POSE_IDX];
  return net_outputs;
}

void model_free(ModelState* s) {
  free(s->output);
  free(s->input_frames);
  frame_free(&s->frame);
  delete s->m;
}

void poly_fit(float *in_pts, float *in_stds, float *out, int valid_len) {
  // References to inputs
  Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, 1> > pts(in_pts, valid_len);
  Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, 1> > std(in_stds, valid_len);
  Eigen::Map<Eigen::Matrix<float, POLYFIT_DEGREE - 1, 1> > p(out, POLYFIT_DEGREE - 1);

  float y0 = pts[0];
  pts = pts.array() - y0;

  // Build Least Squares equations
  Eigen::Matrix<float, Eigen::Dynamic, POLYFIT_DEGREE - 1> lhs = vander.topRows(valid_len).array().colwise() / std.array();
  Eigen::Matrix<float, Eigen::Dynamic, 1> rhs = pts.array() / std.array();

  // Improve numerical stability
  Eigen::Matrix<float, POLYFIT_DEGREE - 1, 1> scale = 1. / (lhs.array()*lhs.array()).sqrt().colwise().sum();
  lhs = lhs * scale.asDiagonal();

  // Solve inplace
  p = lhs.colPivHouseholderQr().solve(rhs);

  // Apply scale to output
  p = p.transpose() * scale.asDiagonal();
  out[3] = y0;
}

void fill_path(cereal::ModelData::PathData::Builder path, const float * data, float valid_len, float prob) {
  float points_arr[TRAJECTORY_SIZE];
  float stds_arr[TRAJECTORY_SIZE];
  float poly_arr[POLYFIT_DEGREE];
  float std;
  float prob;
  float valid_len;

  for (int i=0; i<TRAJECTORY_SIZE; i++) {
    points_arr[i] = data[16*i];
    stds_arr[i] = exp(data[30*33 + 16*i]);
  }
  std = stds_arr[0];
  poly_fit(points_arr, stds_arr, poly_arr, valid_len);

  if (std::getenv("DEBUG")){
    kj::ArrayPtr<const float> stds(&stds_arr[0], ARRAYSIZE(stds_arr));
    path.setStds(stds);

    kj::ArrayPtr<const float> points(&points_arr[0], ARRAYSIZE(points_arr));
    path.setPoints(points);
  }

  kj::ArrayPtr<const float> poly(&poly_arr[0], ARRAYSIZE(poly_arr));
  path.setPoly(poly);
  path.setProb(prob);
  path.setStd(std);
  path.setValidLen(valid_len);
}

void fill_lead(cereal::ModelData::LeadData::Builder lead, const float * data, int mdn_max_idx, int t_offset) {
  const double x_scale = 10.0;
  const double y_scale = 10.0;

  lead.setProb(sigmoid(data[LEAD_MDN_N*MDN_GROUP_SIZE + t_offset]));
  lead.setDist(x_scale * data[mdn_max_idx*MDN_GROUP_SIZE]);
  lead.setStd(x_scale * softplus(data[mdn_max_idx*MDN_GROUP_SIZE + MDN_VALS]));
  lead.setRelY(y_scale * data[mdn_max_idx*MDN_GROUP_SIZE + 1]);
  lead.setRelYStd(y_scale * softplus(data[mdn_max_idx*MDN_GROUP_SIZE + MDN_VALS + 1]));
  lead.setRelVel(data[mdn_max_idx*MDN_GROUP_SIZE + 2]);
  lead.setRelVelStd(softplus(data[mdn_max_idx*MDN_GROUP_SIZE + MDN_VALS + 2]));
  lead.setRelA(data[mdn_max_idx*MDN_GROUP_SIZE + 3]);
  lead.setRelAStd(softplus(data[mdn_max_idx*MDN_GROUP_SIZE + MDN_VALS + 3]));
}

void fill_meta(cereal::ModelData::MetaData::Builder meta, const float * meta_data) {
  kj::ArrayPtr<const float> desire_state(&meta_data[0], DESIRE_LEN);
  meta.setDesireState(desire_state);
  meta.setEngagedProb(meta_data[DESIRE_LEN]);
  meta.setGasDisengageProb(meta_data[DESIRE_LEN + 1]);
  meta.setBrakeDisengageProb(meta_data[DESIRE_LEN + 2]);
  meta.setSteerOverrideProb(meta_data[DESIRE_LEN + 3]);
  kj::ArrayPtr<const float> desire_pred(&meta_data[DESIRE_LEN + OTHER_META_SIZE], DESIRE_PRED_SIZE);
  meta.setDesirePrediction(desire_pred);
}


void model_publish(PubMaster &pm, uint32_t vipc_frame_id, uint32_t frame_id,
                   uint32_t vipc_dropped_frames, float frame_drop, const ModelDataRaw &net_outputs, uint64_t timestamp_eof) {
  uint32_t frame_age = (frame_id > vipc_frame_id) ? (frame_id - vipc_frame_id) : 0;

  MessageBuilder msg;
  auto framed = msg.initEvent(frame_drop < MAX_FRAME_DROP).initModel();
  framed.setFrameId(vipc_frame_id);
  framed.setFrameAge(frame_age);
  framed.setFrameDropPerc(frame_drop * 100);
  framed.setTimestampEof(timestamp_eof);

  // Find the distribution that corresponds to the most probable plan
  int plan_mhp_max_idx = 0;
  for (int i=1; i<PLAN_MHP_N; i++) {
    if (net_outputs.lead[(i + 1)*PLAN_MHP_GROUP_SIZE - 1] >
        net_outputs.lead[(plan_mhp_max_idx + 1)*PLAN_MHP_GROUP_SIZE - 1]) {
      plan_mhp_max_idx = i;
    }
  }
  // x pos at 10s is a good valid_len
  float valid_len = net_outputs.plan[plan_mhp_max_idx*PLAN_MHP_GROUP_SIZE + 15*33];
  // clamp to 5 and MODEL_PATH_DISTANCE
  valid_len = fmin(MODEL_PATH_DISTANCE, fmax(5, valid_len));
  auto lpath = framed.initPath();
  fill_path(lpath, net_outputs.plan[plan_mhp_max_idx*PLAN_MHP_GROUP_SIZE], valid_len);
  
  auto left_lane = framed.initLeftLane();
  fill_path(left_lane, net_outputs.left_lane, true, 1.8);
  auto right_lane = framed.initRightLane();
  fill_path(right_lane, net_outputs.right_lane, true, -1.8);

  // Find the distribution that corresponds to the current lead
  int mdn_max_idx = 0;
  int t_offset = 0;
  for (int i=1; i<LEAD_MDN_N; i++) {
    if (net_outputs.lead[i*MDN_GROUP_SIZE + 8 + t_offset] > net_outputs.lead[mdn_max_idx*MDN_GROUP_SIZE + 8 + t_offset]) {
      mdn_max_idx = i;
    }
  }
  fill_lead(framed.initLead(), net_outputs.lead, mdn_max_idx, t_offset);
  // Find the distribution that corresponds to the lead in 2s
  mdn_max_idx = 0;
  t_offset = 1;
  for (int i=1; i<LEAD_MDN_N; i++) {
    if (net_outputs.lead[i*MDN_GROUP_SIZE + 8 + t_offset] > net_outputs.lead[mdn_max_idx*MDN_GROUP_SIZE + 8 + t_offset]) {
      mdn_max_idx = i;
    }
  }
  fill_lead(framed.initLeadFuture(), net_outputs.lead, mdn_max_idx, t_offset);
  fill_meta(framed.initMeta(), net_outputs.meta);

  pm.send("model", msg);
}

void posenet_publish(PubMaster &pm, uint32_t vipc_frame_id, uint32_t frame_id,
                     uint32_t vipc_dropped_frames, float frame_drop, const ModelDataRaw &net_outputs, uint64_t timestamp_eof) {
  float trans_arr[3];
  float trans_std_arr[3];
  float rot_arr[3];
  float rot_std_arr[3];

  for (int i =0; i < 3; i++) {
    trans_arr[i] = net_outputs.pose[i];
    trans_std_arr[i] = softplus(net_outputs.pose[6 + i]) + 1e-6;

    rot_arr[i] = M_PI * net_outputs.pose[3 + i] / 180.0;
    rot_std_arr[i] = M_PI * (softplus(net_outputs.pose[9 + i]) + 1e-6) / 180.0;
  }

  MessageBuilder msg;
  auto posenetd = msg.initEvent(vipc_dropped_frames < 1).initCameraOdometry();
  kj::ArrayPtr<const float> trans_vs(&trans_arr[0], 3);
  posenetd.setTrans(trans_vs);
  kj::ArrayPtr<const float> rot_vs(&rot_arr[0], 3);
  posenetd.setRot(rot_vs);
  kj::ArrayPtr<const float> trans_std_vs(&trans_std_arr[0], 3);
  posenetd.setTransStd(trans_std_vs);
  kj::ArrayPtr<const float> rot_std_vs(&rot_std_arr[0], 3);
  posenetd.setRotStd(rot_std_vs);

  posenetd.setTimestampEof(timestamp_eof);
  posenetd.setFrameId(vipc_frame_id);

  pm.send("cameraOdometry", msg);
}
