#include "cnn/nodes.h"
#include "cnn/cnn.h"
#include "cnn/rnn.h"
#include "cnn/gru.h"
#include "cnn/lstm.h"
#include "cnn/training.h"
#include "cnn/gpu-ops.h"
#include "cnn/expr.h"

#include "lm.h"
#include "utils.h"
#include "lm-sep-morph.h"

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>

#include <iostream>
#include <fstream>
#include <unordered_map>

#include <fenv.h>

using namespace std;
using namespace cnn;
using namespace cnn::expr;

int main(int argc, char** argv) {
  cnn::Initialize(argc, argv);
  feenableexcept(FE_INVALID | FE_OVERFLOW | FE_DIVBYZERO);

  string vocab_filename = argv[1];  // vocabulary of words/characters
  string morph_filename = argv[2];
  string train_filename = argv[3];
  string test_filename = argv[4];
  unsigned hidden_size = atoi(argv[5]);
  unsigned num_iter = atoi(argv[6]);
  float reg_strength = atof(argv[7]);
  unsigned layers = atoi(argv[8]);
  string lm_model_filename = argv[9];
  string model_outputfilename = argv[10];

  unordered_map<string, unsigned> char_to_id, morph_to_id;
  unordered_map<unsigned, string> id_to_char, id_to_morph;

  ReadVocab(vocab_filename, &char_to_id, &id_to_char);
  unsigned vocab_size = char_to_id.size();
  ReadVocab(morph_filename, &morph_to_id, &id_to_morph);
  unsigned morph_size = morph_to_id.size();

  vector<string> train_data;  // Read the training file in a vector
  ReadData(train_filename, &train_data);

  vector<string> test_data;  // Read the dev file in a vector
  ReadData(test_filename, &test_data);

  LM lm(lm_model_filename, char_to_id, id_to_char);

  vector<Model*> m;
  vector<AdadeltaTrainer> optimizer;

  for (unsigned i = 0; i < morph_size; ++i) {
    m.push_back(new Model());
    AdadeltaTrainer ada(m[i], reg_strength);
    optimizer.push_back(ada);
  }

  unsigned char_size = vocab_size;
  LMSepMorph nn(char_size, hidden_size, vocab_size, layers, morph_size,
                &m, &optimizer);

  // Read the training file and train the model
  double best_score = -1;
  vector<LMSepMorph*> object_list;
  object_list.push_back(&nn);
  for (unsigned iter = 0; iter < num_iter; ++iter) {
    unsigned line_id = 0;
    random_shuffle(train_data.begin(), train_data.end());
    vector<float> loss(morph_size, 0.0f);
    for (string& line : train_data) {
      vector<string> items = split_line(line, '|');
      vector<unsigned> input_ids, target_ids;
      input_ids.clear(); target_ids.clear();

      string input = items[0], output = items[1];
      for (const string& ch : split_line(input, ' ')) {
        input_ids.push_back(char_to_id[ch]);
      }
      for (const string& ch : split_line(output, ' ')) {
        target_ids.push_back(char_to_id[ch]);
      }
      unsigned morph_id = morph_to_id[items[2]];
      loss[morph_id] += nn.Train(morph_id, input_ids, target_ids,
                                 &lm, &optimizer[morph_id]);
      cerr << ++line_id << "\r";
    }

    // Read the test file and output predictions for the words.
    string line;
    double correct = 0, total = 0;
    for (string& line : test_data) {
      vector<string> items = split_line(line, '|');
      vector<unsigned> input_ids, target_ids, pred_target_ids;
      input_ids.clear(); target_ids.clear(); pred_target_ids.clear();

      string input = items[0], output = items[1];
      for (const string& ch : split_line(input, ' ')) {
        input_ids.push_back(char_to_id[ch]);
      }
      for (const string& ch : split_line(output, ' ')) {
        target_ids.push_back(char_to_id[ch]);
      }
      unsigned morph_id = morph_to_id[items[2]];
      EnsembleDecode(morph_id, char_to_id, input_ids, &pred_target_ids, &lm,
                     &object_list);

      string prediction = "";
      for (unsigned i = 0; i < pred_target_ids.size(); ++i) {
        prediction += id_to_char[pred_target_ids[i]];
        if (i != pred_target_ids.size() - 1) {
          prediction += " ";
        }
      }
      if (prediction == output) {
        correct += 1;
      }
      total += 1;
    }
    double curr_score = correct / total;
    cerr << "Iter " << iter + 1 << " " << "Loss: " << accumulate(loss.begin(), loss.end(), 0.);
    cerr << " Prediction Accuracy: " << curr_score << endl;
    if (curr_score > best_score) {
      best_score = curr_score;
      Serialize(model_outputfilename, nn, &m);
    }
  }
  return 1;
}
