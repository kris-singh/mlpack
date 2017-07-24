
#ifndef MLPACK_METHODS_RBM_SPIKE_SLAB_RBM_POLICY_IMPL_HPP
#define MLPACK_METHODS_RBM_SPIKE_SLAB_RBM_POLICY_IMPL_HPP

#include "spike_slab_rbm_policy.hpp"

namespace mlpack{
namespace rbm{

inline SpikeSlabRBMPolicy::SpikeSlabRBMPolicy(const size_t visibleSize,
      const size_t hiddenSize,
      const size_t poolSize,
      const arma::mat& slabPenalty, 
      double radius):
      visibleSize(visibleSize),
      hiddenSize(hiddenSize),
      poolSize(poolSize),
      slabPenalty(slabPenalty),
      radius(radius)
{
  parameter.set_size(visibleSize * hiddenSize * poolSize +
      visibleSize + hiddenSize);

  assert(slabPenalty.n_rows == poolSize);
  assert(slabPenalty.n_cols == hiddenSize);

  spikeMean.set_size(hiddenSize, 1);
  spikeSamples.set_size(hiddenSize, 1);
  slabMean.set_size(poolSize, hiddenSize);
};

// Reset function
inline void SpikeSlabRBMPolicy::Reset()
{
  weight = arma::cube(parameter.memptr(), visibleSize, poolSize, hiddenSize,
      false, false);

  spikeBias = arma::mat(parameter.memptr() + weight.n_elem, hiddenSize, 1,
      false, false);

  visiblePenalty = arma::mat(parameter.memptr() + weight.n_elem +
      spikeBias.n_elem, visibleSize, 1, false, false);
}

/**
 * Free energy of the spike and slab variable
 * the free energy of the ssRBM is given my
 * $v^t$$\Delta$v - $\sum_{i=1}^N$ 
 * $\log{ \sqrt{\frac{(-2\pi)^K}{\prod_{m=1}^{K}(\alpha_i)_m}}}$ -
 * $\sum_{i=1}^N \log(1+\exp( b_i +
 * \sum_{m=1}^k \frac{(v(w_i)_m^t)^2}{2(\alpha_i)_m})$
 *
 * @param input the visible layer
 */ 
inline double SpikeSlabRBMPolicy::FreeEnergy(arma::mat&& input)
{
  assert(input.n_rows == visibleSize);
  assert(input.n_cols == 1);

  double freeEnergy = 0.5 * arma::as_scalar(input.t() *
    arma::diagmat(visiblePenalty) * input);

  for (size_t i = 0; i < hiddenSize; i++)
  {
    for (size_t k = 0; k < poolSize; k++)
      freeEnergy -= 0.5 * std::log(2.0 * M_PI / slabPenalty(k, i));
  }

  for (size_t i = 0; i < hiddenSize; i++)
  {
    double sum = 0;

    for (size_t k = 0; k < poolSize; k++)
    {
      sum += arma::as_scalar(input.t() * weight.slice(i).col(k)) *
          arma::as_scalar(input.t() * weight.slice(i).col(k)) /
          (2.0 * slabPenalty(k, i));
    }

    freeEnergy -= SoftplusFunction::Fn(spikeBias(i) + sum);
  }

  return freeEnergy;
}

inline double SpikeSlabRBMPolicy::Evaluate(arma::mat& /*predictors*/,
    size_t /*i*/)
{
  // Return 0 here since we don't have evaluate in case of persistence
  return 0;
}

/**
 * Gradient function calculates the gradient for the spike and
 * slab RBM.
 *
 * @param input the visible input
 * @param output the computed gradient
 */
inline void SpikeSlabRBMPolicy::PositivePhase(arma::mat&& input,
    arma::mat&& gradient)
{
  arma::cube weightGrad = arma::cube(gradient.memptr(), visibleSize, poolSize,
      hiddenSize, false, false);

  arma::mat spikeBiasGrad = arma::mat(gradient.memptr() + weightGrad.n_elem,
      hiddenSize, 1, false, false);

  arma::mat visiblePenaltyGrad = arma::mat(gradient.memptr() +
      weightGrad.n_elem + spikeBiasGrad.n_elem, visibleSize, 1, false, false);

  SpikeMean(std::move(input), std::move(spikeMean));
  SampleSpike(std::move(spikeMean), std::move(spikeSamples));
  SlabMean(std::move(input), std::move(spikeSamples), std::move(slabMean));

  // positive weight gradient
  for (size_t i = 0 ; i < hiddenSize; i++)
    weightGrad.slice(i) = input * slabMean.col(i).t() * spikeMean(i);

  // positive hidden bias gradient
  for (size_t i = 0; i < hiddenSize; i++)
    spikeBiasGrad(i) = spikeMean(i);
  // positive lambda bias
  for (size_t i = 0 ; i < visibleSize; i++)
    visiblePenaltyGrad(i) = -0.5 * input(i) * input(i);
}

inline void SpikeSlabRBMPolicy::NegativePhase(arma::mat&& negativeSamples,
    arma::mat&& gradient)
{
  arma::cube weightGrad = arma::cube(gradient.memptr(), visibleSize, poolSize,
      hiddenSize, false, false);

  arma::mat spikeBiasGrad = arma::mat(gradient.memptr() + weightGrad.n_elem,
      hiddenSize, 1, false, false);

  arma::mat visiblePenaltyGrad = arma::mat(gradient.memptr() +
      weightGrad.n_elem + spikeBiasGrad.n_elem, visibleSize, 1, false, false);

  SpikeMean(std::move(negativeSamples), std::move(spikeMean));
  SampleSpike(std::move(spikeMean), std::move(spikeSamples));
  SlabMean(std::move(negativeSamples), std::move(spikeSamples),
      std::move(slabMean));

  // positive weight gradient
  for (size_t i = 0 ; i < hiddenSize; i++)
    weightGrad.slice(i) = negativeSamples * slabMean.col(i).t() * spikeMean(i);

  // positive hidden bias gradient
  for (size_t i = 0; i < hiddenSize; i++)
    spikeBiasGrad(i) = spikeMean(i);
  // positive lambda bias
  for (size_t i = 0 ; i < visibleSize; i++)
    visiblePenaltyGrad(i) = -0.5 * negativeSamples(i) * negativeSamples(i);
}

inline void SpikeSlabRBMPolicy::SpikeMean(arma::mat&& visible,
    arma::mat&& spikeMean)
{
  assert(visible.n_rows == visibleSize);
  assert(visible.n_cols == 1);

  assert(spikeMean.n_rows == hiddenSize);
  assert(spikeMean.n_cols == 1);

  for (size_t i = 0; i < hiddenSize; i++)
  {
    spikeMean(i) = LogisticFunction::Fn(0.5 * arma::as_scalar(visible.t() *
        weight.slice(i) * arma::diagmat(slabPenalty.col(i)).i() *
        weight.slice(i).t() * visible) + spikeBias(i));
  }
}

inline void SpikeSlabRBMPolicy::SampleSpike(arma::mat&& spikeMean,
    arma::mat&& spike)
{
  assert(spikeMean.n_rows == hiddenSize);
  assert(spikeMean.n_cols == 1);

  assert(spike.n_rows == hiddenSize);
  assert(spike.n_cols == 1);

  for (size_t i = 0; i < hiddenSize; i++)
    spike(i) = math::RandBernoulli(spikeMean(i));
}

inline void SpikeSlabRBMPolicy::SlabMean(arma::mat&& visible, arma::mat&& spike,
    arma::mat&& slabMean)
{
  assert(visible.n_rows == visibleSize);
  assert(visible.n_cols == 1);

  assert(spike.n_rows == hiddenSize);
  assert(spike.n_cols == 1);

  assert(slabMean.n_rows == poolSize);
  assert(slabMean.n_cols == hiddenSize);

  assert(weight.n_rows == visibleSize);
  assert(weight.n_cols == poolSize);

  assert(slabPenalty.n_rows == poolSize);
  assert(slabPenalty.n_cols == hiddenSize);

  for (size_t i = 0; i < hiddenSize; i++)
  {
    slabMean.col(i) = spike(i) * arma::diagmat(slabPenalty.col(i)).i() *
        weight.slice(i).t() * visible;
  }
}


inline void SpikeSlabRBMPolicy::SampleSlab(arma::mat&& slabMean,
    arma::mat&& slab)
{
  assert(slabMean.n_rows == poolSize);
  assert(slabMean.n_cols == hiddenSize);

  assert(slab.n_rows == poolSize);
  assert(slab.n_cols == hiddenSize);

  assert(slabPenalty.n_rows == poolSize);
  assert(slabPenalty.n_cols == hiddenSize);

  for (size_t i = 0; i < hiddenSize; i++)
  {
    for (size_t j = 0; j < poolSize; j++)
    {
      assert(slabPenalty(j, i) > 0);
      slab(j, i) = math::RandNormal(slabMean(j, i), 1.0 / slabPenalty(j, i));
    }
  }
}

inline void SpikeSlabRBMPolicy::VisibleMean(arma::mat&& input,
    arma::mat&& output)
{
  assert(input.n_elem == hiddenSize + poolSize * hiddenSize);
  output.set_size(visibleSize, 1);

  arma::mat spike(input.memptr(), hiddenSize, 1, false, false);
  arma::mat slab(input.memptr() + hiddenSize, poolSize, hiddenSize, false,
      false);

  for (size_t i = 0; i < hiddenSize; i++)
    output += weight.slice(i) * slab.col(i) * spike(i);

  output = arma::diagmat(visiblePenalty).i() * output;
}

inline void SpikeSlabRBMPolicy::HiddenMean(arma::mat&& input,
    arma::mat&& output)
{
  assert(input.n_elem == visibleSize);
  output.set_size(hiddenSize + poolSize * hiddenSize, 1);

  arma::mat spike(output.memptr(), hiddenSize, 1, false, false);
  arma::mat slab(output.memptr() + hiddenSize, poolSize, hiddenSize, false,
      false);

  SpikeMean(std::move(input), std::move(spike));
  SampleSpike(std::move(spike), std::move(spikeSamples));
  SlabMean(std::move(input), std::move(spikeSamples), std::move(slab));
}


inline void SpikeSlabRBMPolicy::SampleVisible(arma::mat&& input,
    arma::mat&& output)
{
  const size_t numMaxTrials = 10;

  VisibleMean(std::move(input), std::move(output));

  for (size_t k = 0; k < numMaxTrials; k++)
  {
    for (size_t i = 0; i < visibleSize; i++)
    {
      assert(visiblePenalty[i] > 0);
      output(i) = math::RandNormal(output(i), 1.0 / visiblePenalty[i]);
    }
    if (arma::norm(output) < radius)
      break;
  }
}

inline void SpikeSlabRBMPolicy::SampleHidden(arma::mat&& input,
    arma::mat&& output)
{
  assert(input.n_elem == visibleSize);
  output.set_size(hiddenSize + poolSize * hiddenSize, 1);

  arma::mat spike(output.memptr(), hiddenSize, 1, false, false);
  arma::mat slab(output.memptr() + hiddenSize, poolSize, hiddenSize, false,
      false);

  SpikeMean(std::move(input), std::move(spike));
  SampleSpike(std::move(spike), std::move(spike));
  SlabMean(std::move(input), std::move(spike), std::move(slab));
  SampleSlab(std::move(slab), std::move(slab));
}

template<typename Archive>
void SpikeSlabRBMPolicy::Serialize(Archive& ar,
    const unsigned int /* version */)
{
  ar & data::CreateNVP(visibleSize, "visibleSize");
  ar & data::CreateNVP(hiddenSize, "hiddenSize");
  ar & data::CreateNVP(poolSize, "poolSize");
  ar & data::CreateNVP(parameter, "parameter");
  ar & data::CreateNVP(weight, "weight");
  ar & data::CreateNVP(spikeBias, "spikeBias");
  ar & data::CreateNVP(slabPenalty, "slabPenalty");
  ar & data::CreateNVP(radius, "radius");
  ar & data::CreateNVP(visiblePenalty, "visiblePenalty");

  if (Archive::is_loading::value)
  {
    spikeMean.set_size(hiddenSize, 1);
    spikeSamples.set_size(hiddenSize, 1);
    slabMean.set_size(poolSize, hiddenSize);
  }
}

} // namespace rbm
} // namespace mlpack


#endif // MLPACK_METHODS_RBM_SPIKE_SLAB_RBM_POLICY_IMPL_HPP
