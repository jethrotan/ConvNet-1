/*
Copyright (C) 2014 Sergey Demyanov. 
contact: sergey@demyanov.net
http://www.demyanov.net

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "layer_c.h"

LayerConv::LayerConv() {
  type_ = "c";
  function_ = "relu";  
  batchsize_ = 0;
  sum_width_ = 1;
}

void LayerConv::Init(const mxArray *mx_layer, Layer *prev_layer) {
  
  mexAssert(prev_layer->type_ != "f", "The 'c' type layer cannot be after 'f' type layer");
  numdim_ = prev_layer->numdim_;
  length_prev_ = prev_layer->outputmaps_;
  mexAssert(mexIsField(mx_layer, "outputmaps"), "The 'c' type layer must contain the 'outputmaps' field");
  ftype outputmaps = mexGetScalar(mexGetField(mx_layer, "outputmaps"));
  mexAssert(1 <= outputmaps, "Outputmaps on the 'i' layer must be greater or equal to 1");
  outputmaps_ = (size_t) outputmaps;  
  if (mexIsField(mx_layer, "function")) {
    function_ = mexGetString(mexGetField(mx_layer, "function"));
    mexAssert(function_ == "soft" || function_ == "sigm" || function_ == "relu", 
      "Unknown function for the 'c' layer");    
  }
  mexAssert(mexIsField(mx_layer, "filtersize"), "The 'c' type layer must contain the 'filtersize' field");
  std::vector<ftype> filtersize = mexGetVector(mexGetField(mx_layer, "filtersize"));
  mexAssert(filtersize.size() == numdim_, "Filters and maps must be the same dimensionality");
  filtersize_.resize(numdim_);    
  for (size_t i = 0; i < numdim_; ++i) {
    mexAssert(1 <= filtersize[i], "Filtersize on the 'c' layer must be greater or equal to 1");
    filtersize_[i] = (size_t) filtersize[i];    
  }
  #if COMP_REGIME == 2	// GPU
    mexAssert(filtersize_[0] == filtersize_[1], "In the GPU version the filtersize should be squared on all layers");
  #endif
  padding_.assign(numdim_, 0);
  if (mexIsField(mx_layer, "padding")) {
    std::vector<ftype> padding = mexGetVector(mexGetField(mx_layer, "padding"));
    mexAssert(padding.size() == numdim_, "Padding vector has the wrong length");
    for (size_t i = 0; i < numdim_; ++i) {
      mexAssert(0 <= padding[i] && padding[i] <= filtersize_[i] - 1, "Padding on the 'c' layer must be in the range [0, filtersize-1]");
      padding_[i] = (size_t) padding[i];
    }
    #if COMP_REGIME == 2	// GPU
      mexAssert(padding_[0] == padding_[1], "In the GPU version the padding should be squared on all layers");
    #endif    
  }
  mapsize_.resize(numdim_);
  length_ = outputmaps_;
  size_t minsize_ = prev_layer->mapsize_[0] + 2*padding_[0] - filtersize_[0] + 1;
  for (size_t i = 0; i < numdim_; ++i) {
    mapsize_[i] = prev_layer->mapsize_[i] + 2*padding_[i] - filtersize_[i] + 1;
    mexAssert(1 <= mapsize_[i], "Mapsize on the 'c' layer must be greater than 1");
    if (mapsize_[i] < minsize_) minsize_ = mapsize_[i];
    length_ *= mapsize_[i];
  }  
  if (mexIsField(mx_layer, "sumwidth")) {
    ftype sum_width = mexGetScalar(mexGetField(mx_layer, "sumwidth"));
    mexAssert(1 <= sum_width && sum_width <= minsize_, "Sumwidth must be in the range [0, min(mapsize)]");
    sum_width_ = (size_t) sum_width;
  } else {
    if (sum_width_ > minsize_) sum_width_ = minsize_;
  }
}
    
void LayerConv::Forward(Layer *prev_layer, int passnum) {
  batchsize_ = prev_layer->batchsize_;
  activ_mat_.resize(batchsize_, length_);  
  #if COMP_REGIME != 2
    std::vector< std::vector<Mat> > prev_activ, filters, activ;    
    InitMaps(prev_layer->activ_mat_, prev_layer->mapsize_, prev_activ);
    InitMaps(weights_.get(), filtersize_, filters);
    InitMaps(activ_mat_, mapsize_, activ);    
    activ_mat_.assign(0);    
    #if COMP_REGIME == 1
      #pragma omp parallel for
    #endif    
    for (int k = 0; k < batchsize_; ++k) {    
      for (size_t i = 0; i < outputmaps_; ++i) {        
        for (size_t j = 0; j < prev_layer->outputmaps_; ++j) {
          Mat act_mat(mapsize_);
          Filter(prev_activ[k][j], filters[i][j], padding_, false, act_mat);
          activ[k][i] += act_mat;        
        }      
      }    
    }
  #else // GPU
    FilterActs(prev_layer->activ_mat_, weights_.get(), activ_mat_, 
               prev_layer->mapsize_, padding_[0]);    
  #endif
  if (passnum == 0 || passnum == 1) {
    mexAssert(activ_mat_.order() == false, "activ_mat_.order() should be false");
    activ_mat_.reshape(activ_mat_.size1() * activ_mat_.size2() / outputmaps_, outputmaps_);
    activ_mat_.AddVect(biases_.get(), 1);
    activ_mat_.reshape(batchsize_, activ_mat_.size1() * activ_mat_.size2() / batchsize_);            
  }  
  activ_mat_.Validate();
}

void LayerConv::Backward(Layer *prev_layer) {
  prev_layer->deriv_mat_.resize(prev_layer->batchsize_, prev_layer->length_);
  #if COMP_REGIME != 2
    std::vector< std::vector<Mat> > prev_deriv, filters, deriv;
    InitMaps(prev_layer->deriv_mat_, prev_layer->mapsize_, prev_deriv); 
    InitMaps(weights_.get(), filtersize_, filters);
    InitMaps(deriv_mat_, mapsize_, deriv);
    std::vector<size_t> padding_der(numdim_);
    for (size_t i = 0; i < numdim_; ++i) {
      padding_der[i] = filtersize_[i] - 1 - padding_[i];
    }
    prev_layer->deriv_mat_.assign(0);    
    #if COMP_REGIME == 1
      #pragma omp parallel for
    #endif  
    for (int k = 0; k < batchsize_; ++k) {    
      for (size_t i = 0; i < outputmaps_; ++i) {      
        for (size_t j = 0; j < prev_layer->outputmaps_; ++j) {                        
          Mat der_mat(prev_layer->mapsize_);
          Filter(deriv[k][i], filters[i][j], padding_der, true, der_mat);
          prev_deriv[k][j] += der_mat;        
        }      
      }        
    }
  #else // GPU       
    ImgActs(deriv_mat_, weights_.get(), prev_layer->deriv_mat_, 
            prev_layer->mapsize_, padding_[0]);        
  #endif
  prev_layer->deriv_mat_.Validate();
}

void LayerConv::CalcWeights(Layer *prev_layer, int passnum) {
  
  if (passnum < 2) return;
  Mat weights_der;
  if (passnum == 2) {
    weights_der.attach(weights_.der());
  }
  #if COMP_REGIME != 2
    std::vector< std::vector<Mat> > prev_activ, filters_der, deriv;
    InitMaps(prev_layer->activ_mat_, prev_layer->mapsize_, prev_activ); 
    InitMaps(weights_der, filtersize_, filters_der);
    InitMaps(deriv_mat_, mapsize_, deriv);
    for (size_t i = 0; i < outputmaps_; ++i) {
      for (size_t j = 0; j < prev_layer->outputmaps_; ++j) {                
        Mat fil_der(filtersize_);
        fil_der.assign(0);      
        #if COMP_REGIME == 1
          #pragma omp parallel for
        #endif
        for (int k = 0; k < batchsize_; ++k) {
          Mat ker_mat(filtersize_);
          Filter(prev_activ[k][j], deriv[k][i], padding_, false, ker_mat);        
          #if COMP_REGIME == 1
            #pragma omp critical
          #endif
          fil_der += ker_mat;          
        }
        filters_der[i][j] = fil_der;        
      }        
    }    
  #else // GPU
    WeightActs(prev_layer->activ_mat_, deriv_mat_, weights_der,
               prev_layer->mapsize_, padding_[0], filtersize_[0], sum_width_, tmpbuf_der_);        
  #endif  
  if (passnum == 2) {
    mexAssert(deriv_mat_.order() == false, "deriv_mat_.order() should be false");
    deriv_mat_.reshape(deriv_mat_.size1() * deriv_mat_.size2() / outputmaps_, outputmaps_);
    Sum(deriv_mat_, biases_.der(), 1);
    deriv_mat_.reshape(batchsize_, deriv_mat_.size1() * deriv_mat_.size2() / batchsize_);        
    biases_.der() /= batchsize_;      
    biases_.der().Validate();
  }
  weights_der /= batchsize_;  
  weights_der.Validate();   
}

void LayerConv::InitWeights(Weights &weights, size_t &offset, bool isgen) {  
  size_t numel = 1;
  for (size_t i = 0; i < numdim_; ++i) {
    numel *= filtersize_[i];
  }
  size_t pixel_num = length_prev_ * numel;
  weights_.Attach(weights, offset, outputmaps_, pixel_num, false);  
  offset += outputmaps_ * pixel_num;
  if (isgen) {  
    size_t fan_in = pixel_num;
    size_t fan_out = outputmaps_ * numel;
    ftype rand_coef = 2 * sqrt((ftype) 6 / (fan_in + fan_out));
    (weights_.get().rand() -= 0.5) *= rand_coef;        
  }
  biases_.Attach(weights, offset, 1, outputmaps_, kMatlabOrder);  
  offset += outputmaps_;  
  if (isgen) {
    biases_.get().assign(0);
  }
}  

void LayerConv::GetWeights(Mat &weights, size_t &offset) const {
  Mat weights_mat;
  size_t pixel_num = weights_.get().size2();
  weights_mat.attach(weights, offset, outputmaps_, pixel_num, false);
  weights_mat = weights_.get();
  offset += outputmaps_ * pixel_num;
  
  Mat biases_mat;
  biases_mat.attach(weights, offset, 1, outputmaps_, kMatlabOrder);
  biases_mat = biases_.get();
  offset += outputmaps_;
}

size_t LayerConv::NumWeights() const {
  size_t num_weights = length_prev_;
  for (size_t i = 0; i < numdim_; ++i) {
    num_weights *= filtersize_[i];
  }
  return (num_weights + 1) * outputmaps_; // +1 for biases
}