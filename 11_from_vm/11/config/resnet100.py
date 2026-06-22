import os
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import backend as K

BATCH_NORM_DECAY = 0.9
BATCH_NORM_EPSILON = 1e-5
CONV_KERNEL_INITIALIZER = tf.keras.initializers.VarianceScaling(scale=2.0, mode="fan_out", distribution="truncated_normal")
# CONV_KERNEL_INITIALIZER = 'glorot_uniform'

def batchnorm_with_activation(inputs, activation="relu", zero_gamma=False, name=""):
    """Performs a batch normalization followed by an activation."""
    bn_axis = 3 if K.image_data_format() == "channels_last" else 1
    gamma_initializer = tf.zeros_initializer() if zero_gamma else tf.ones_initializer()
    nn = keras.layers.BatchNormalization(
        axis=bn_axis,
        momentum=BATCH_NORM_DECAY,
        epsilon=BATCH_NORM_EPSILON,
        gamma_initializer=gamma_initializer,
        name=name + "bn",
    )(inputs)
    if activation:
        act_name = name + activation
        if activation.lower() == "prelu":
            nn = keras.layers.PReLU(shared_axes=[1, 2], alpha_initializer=tf.initializers.Constant(0.25), name=act_name)(nn)
        else:
            nn = keras.layers.Activation(activation=activation, name=act_name)(nn)
    return nn

def get_zero_padding2d_name():
    if not hasattr(get_zero_padding2d_name, "counter"):
        get_zero_padding2d_name.counter = 0
    if get_zero_padding2d_name.counter == 0:
        name = "zero_padding2d"
    else:
        name = f"zero_padding2d_{get_zero_padding2d_name.counter}"
    get_zero_padding2d_name.counter += 1
    return name

def conv2d_no_bias(inputs, filters, kernel_size, strides=1, padding="VALID", name=""):
    if padding.upper() == "SAME":
        inputs = keras.layers.ZeroPadding2D(((1, 1), (1, 1)), name = get_zero_padding2d_name())(inputs)
    return keras.layers.Conv2D(
        filters,
        kernel_size,
        strides=strides,
        padding="VALID",
        use_bias=False,
        kernel_initializer=CONV_KERNEL_INITIALIZER,
        name=name + "conv",
    )(inputs)


def se_module(inputs, se_ratio=4, name=""):
    channel_axis = 1 if K.image_data_format() == "channels_first" else -1
    h_axis, w_axis = [2, 3] if K.image_data_format() == "channels_first" else [1, 2]

    filters = inputs.shape[channel_axis]
    # reduction = _make_divisible(filters // se_ratio, 8)
    reduction = filters // se_ratio
    se = tf.reduce_mean(inputs, [h_axis, w_axis], keepdims=True)
    se = conv2d_no_bias(se, reduction, 1, name=name + "_1_")
    se = keras.layers.Activation("relu")(se)
    se = conv2d_no_bias(se, filters, 1, name=name + "_2_")
    se = keras.layers.Activation("sigmoid")(se)
    return keras.layers.Multiply()([inputs, se])


def block(inputs, out_channel, strides=1, activation="relu", use_se=False, conv_shortcut=False, name=""):
    if conv_shortcut:
        shortcut = conv2d_no_bias(inputs, out_channel, 1, strides=strides, name=name + "_shortcut_")
        shortcut = batchnorm_with_activation(shortcut, activation=None, name=name + "_shortcut_")
    else:
      shortcut = inputs if strides == 1 else keras.layers.MaxPooling2D(1, strides=strides)(inputs)

    nn = batchnorm_with_activation(inputs, activation=None, name=name + "_1_")
    nn = conv2d_no_bias(nn, out_channel, 3, strides=1, padding="same", name=name + "_1_")
    nn = batchnorm_with_activation(nn, activation=activation, name=name + "_2_")
    nn = conv2d_no_bias(nn, out_channel, 3, strides=strides, padding="same", name=name + "_2_")
    nn = batchnorm_with_activation(nn, activation=None, name=name + "_3_")
    if use_se:
        nn = se_module(nn, se_ratio=16, name=name + "_se")
    return keras.layers.Add(name=name + "_add")([shortcut, nn])

def resnet_stack_fn(inputs, out_channels, depthes, use_se=False, use_max_pool=False, strides=2, activation="relu"):
    nn = inputs
    use_ses = use_se if isinstance(use_se, (list, tuple)) else [use_se] * len(out_channels)
    for id, (out_channel, depth, use_se) in enumerate(zip(out_channels, depthes, use_ses)):
        name = "stack" + str(id + 1)
        conv_shortcut = False if use_max_pool or inputs.shape[-1] == out_channel else True
        # print(f"{conv_shortcut = }, {use_max_pool = }")
        nn = block(nn, out_channel, strides, activation, use_se, conv_shortcut, name=name + "_block1")
        for ii in range(2, depth + 1):
            nn = block(nn, out_channel, 1, activation, use_se, False, name=name + "_block" + str(ii))
    return nn

def ResNet(input_shape, stack_fn, classes=1000, activation="relu", model_name="resnet", **kwargs):
    img_input = keras.layers.Input(shape=input_shape, name = "input_1")
    nn = conv2d_no_bias(img_input, 64, 3, strides=1, padding="SAME", name="0_")
    nn = batchnorm_with_activation(nn, activation=activation, name="0_")

    nn = stack_fn(nn)

    if classes > 0:
        nn = keras.layers.GlobalAveragePooling2D(name="avg_pool")(nn)
        nn = keras.layers.Dense(classes, activation="softmax", name="predictions")(nn)

    model = tf.keras.models.Model(img_input, nn, name=model_name)
    return model

def ResNet100(input_shape, classes=1000, activation="relu", use_se=False, use_max_pool=False, model_name="ResNet100", **kwargs):
    out_channels = [64, 128, 256, 512]
    depthes = [3, 13, 30, 3]
    stack_fn = lambda nn: resnet_stack_fn(nn, out_channels, depthes, use_se, use_max_pool, activation=activation)
    return ResNet(input_shape, stack_fn, classes, activation, model_name=model_name, **kwargs)

def __init_model_from_name__(name, input_shape=(112, 112, 3), weights="imagenet", **kwargs):
    name_lower = name.lower()
    """ Basic model """
    if "r18" in name_lower or "r34" in name_lower or "r50" in name_lower or "r100" in name_lower or "r101" in name_lower:
        use_se = True if name_lower.startswith("se_") else False
        model_name = "ResNet" + name_lower[4:] if use_se else "ResNet" + name_lower[1:]
        use_se = kwargs.pop("use_se", use_se)
        xx = ResNet100(input_shape=input_shape, classes=0, use_se=use_se, model_name=model_name, **kwargs)
    else:
        return None
    xx.trainable = True
    return xx

def buildin_models(
    stem_model,
    dropout=1,
    emb_shape=512,
    input_shape=(112, 112, 3),
    output_layer="GDC",
    bn_momentum=0.99,
    bn_epsilon=0.001,
    add_pointwise_conv=False,
    pointwise_conv_act="relu",
    use_bias=False,
    scale=True,
    weights="imagenet",
    **kwargs
):
    if isinstance(stem_model, str):
        xx = __init_model_from_name__(stem_model, input_shape, weights, **kwargs)
        name = stem_model
    else:
        name = stem_model.name
        xx = stem_model

    if bn_momentum != 0.99 or bn_epsilon != 0.001:
        print(">>>> Change BatchNormalization momentum and epsilon default value.")
        for ii in xx.layers:
            if isinstance(ii, keras.layers.BatchNormalization):
                ii.momentum, ii.epsilon = bn_momentum, bn_epsilon
        xx = keras.models.clone_model(xx)

    inputs = xx.inputs[0]
    nn = xx.outputs[0]

    if add_pointwise_conv:  # Model using `pointwise_conv + GDC` / `pointwise_conv + E` is smaller than `E`
        filters = nn.shape[-1] // 2 if add_pointwise_conv == -1 else 512  # Compitable with previous models...
        nn = keras.layers.Conv2D(filters, 1, use_bias=False, padding="valid", name="pw_conv")(nn)
        # nn = keras.layers.Conv2D(nn.shape[-1] // 2, 1, use_bias=False, padding="valid", name="pw_conv")(nn)
        nn = keras.layers.BatchNormalization(momentum=bn_momentum, epsilon=bn_epsilon, name="pw_bn")(nn)
        if pointwise_conv_act.lower() == "prelu":
            nn = keras.layers.PReLU(shared_axes=[1, 2], name="pw_" + pointwise_conv_act)(nn)
        else:
            nn = keras.layers.Activation(pointwise_conv_act, name="pw_" + pointwise_conv_act)(nn)

    if output_layer == "E":
        """Fully Connected"""
        nn = keras.layers.BatchNormalization(momentum=bn_momentum, epsilon=bn_epsilon, name="E_batchnorm")(nn)
        if dropout > 0 and dropout < 1:
            nn = keras.layers.Dropout(dropout, name="dropout")(nn)
        nn = keras.layers.Flatten(name="E_flatten")(nn)
        nn = keras.layers.Dense(emb_shape, use_bias=use_bias, kernel_initializer="glorot_normal", name="E_dense")(nn)
        # ----------------------------------------------
        # nn = keras.layers.Reshape([1, 1, -1])(nn)  # expand_dims to 4D again for applying BatchNormalization
        # ----------------------------------------------

    # `fix_gamma=True` in MXNet means `scale=False` in Keras
    embedding = keras.layers.BatchNormalization(momentum=bn_momentum, epsilon=bn_epsilon, scale=scale, name="pre_embedding")(nn)
    # ------------------------------------------------
    # embedding = keras.layers.Flatten()(embedding)
    # ------------------------------------------------
    embedding_fp32 = keras.layers.Activation("linear", dtype="float32", name="embedding")(embedding)

    basic_model = keras.models.Model(inputs, embedding_fp32, name=xx.name)
    return basic_model

def add_l2_regularizer_2_model(model, weight_decay, custom_objects={}, apply_to_batch_normal=False, apply_to_bias=False):
    # https://github.com/keras-team/keras/issues/2717#issuecomment-456254176
    if 0:
        regularizers_type = {}
        for layer in model.layers:
            rrs = [kk for kk in layer.__dict__.keys() if "regularizer" in kk and not kk.startswith("_")]
            if len(rrs) != 0:
                # print(layer.name, layer.__class__.__name__, rrs)
                if layer.__class__.__name__ not in regularizers_type:
                    regularizers_type[layer.__class__.__name__] = rrs
        print(regularizers_type)

    for layer in model.layers:
        attrs = []
        if isinstance(layer, keras.layers.Dense) or isinstance(layer, keras.layers.Conv2D):
            # print(">>>> Dense or Conv2D", layer.name, "use_bias:", layer.use_bias)
            attrs = ["kernel_regularizer"]
            if apply_to_bias and layer.use_bias:
                attrs.append("bias_regularizer")
        elif isinstance(layer, keras.layers.DepthwiseConv2D):
            # print(">>>> DepthwiseConv2D", layer.name, "use_bias:", layer.use_bias)
            attrs = ["depthwise_regularizer"]
            if apply_to_bias and layer.use_bias:
                attrs.append("bias_regularizer")
        elif isinstance(layer, keras.layers.SeparableConv2D):
            # print(">>>> SeparableConv2D", layer.name, "use_bias:", layer.use_bias)
            attrs = ["pointwise_regularizer", "depthwise_regularizer"]
            if apply_to_bias and layer.use_bias:
                attrs.append("bias_regularizer")
        elif apply_to_batch_normal and isinstance(layer, keras.layers.BatchNormalization):
            # print(">>>> BatchNormalization", layer.name, "scale:", layer.scale, ", center:", layer.center)
            if layer.center:
                attrs.append("beta_regularizer")
            if layer.scale:
                attrs.append("gamma_regularizer")
        elif apply_to_batch_normal and isinstance(layer, keras.layers.PReLU):
            # print(">>>> PReLU", layer.name)
            attrs = ["alpha_regularizer"]

        for attr in attrs:
            if hasattr(layer, attr) and layer.trainable:
                setattr(layer, attr, keras.regularizers.L2(weight_decay / 2))

    # So far, the regularizers only exist in the model config. We need to
    # reload the model so that Keras adds them to each layer's losses.
    # temp_weight_file = "tmp_weights.h5"
    # model.save_weights(temp_weight_file)
    # out_model = keras.models.model_from_json(model.to_json(), custom_objects=custom_objects)
    # out_model.load_weights(temp_weight_file, by_name=True)
    # os.remove(temp_weight_file)
    # return out_model
    return keras.models.clone_model(model)


def replace_ReLU_with_PReLU(model, target_activation="PReLU", **kwargs):
    from tensorflow.keras.layers import ReLU, PReLU, Activation

    def convert_ReLU(layer):
        # print(layer.name)
        if isinstance(layer, ReLU) or (isinstance(layer, Activation) and layer.activation == keras.activations.relu):
            if target_activation == "PReLU":
                layer_name = layer.name.replace("_relu", "_prelu")
                print(">>>> Convert ReLU:", layer.name, "-->", layer_name)
                # Default initial value in mxnet and pytorch is 0.25
                return PReLU(shared_axes=[1, 2], alpha_initializer=tf.initializers.Constant(0.25), name=layer_name, **kwargs)
            elif isinstance(target_activation, str):
                layer_name = layer.name.replace("_relu", "_" + target_activation)
                print(">>>> Convert ReLU:", layer.name, "-->", layer_name)
                return Activation(activation=target_activation, name=layer_name, **kwargs)
            else:
                act_class_name = target_activation.__name__
                layer_name = layer.name.replace("_relu", "_" + act_class_name)
                print(">>>> Convert ReLU:", layer.name, "-->", layer_name)
                return target_activation(**kwargs)
        return layer

    input_tensors = keras.layers.Input(model.input_shape[1:])
    return keras.models.clone_model(model, input_tensors=input_tensors, clone_function=convert_ReLU)
    return keras.layers.Conv2D(
        filters,
        kernel_size,
        strides=strides,
        padding="VALID",
        use_bias=False,
        kernel_initializer=CONV_KERNEL_INITIALIZER,
        name=name + "conv",
    )(inputs)


def build_adaface():
  model = buildin_models("r100", dropout = 0, emb_shape = 512, output_layer = "E")
  model = add_l2_regularizer_2_model(model, weight_decay=5e-4, apply_to_batch_normal=False)
  model = replace_ReLU_with_PReLU(model)
  # Keep weight path portable across OS and Docker by resolving relative to this file.
  weights_path = os.path.join(os.path.dirname(__file__), "r100_AdaFace_glint360k.h5")
  model.load_weights(weights_path)
  return model
