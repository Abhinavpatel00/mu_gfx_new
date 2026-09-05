module tonemapping;

//////////////////////////////////////////////////////////
// ACES (Narkowicz 2015)
//////////////////////////////////////////////////////////

public float3 aces(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;

    return clamp((x * (a * x + b)) /
                 (x * (c * x + d) + e), 0.0, 1.0);
}

public float aces(float x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;

    return clamp((x * (a * x + b)) /
                 (x * (c * x + d) + e), 0.0, 1.0);
}

//////////////////////////////////////////////////////////
// Reinhard
//////////////////////////////////////////////////////////

public float3 reinhard(float3 x)
{
    return x / (1.0 + x);
}

public float reinhard(float x)
{
    return x / (1.0 + x);
}

//////////////////////////////////////////////////////////
// Reinhard2
//////////////////////////////////////////////////////////

public float3 reinhard2(float3 x)
{
    const float L_white = 4.0;
    return (x * (1.0 + x / (L_white * L_white))) /
           (1.0 + x);
}

public float reinhard2(float x)
{
    const float L_white = 4.0;
    return (x * (1.0 + x / (L_white * L_white))) /
           (1.0 + x);
}

//////////////////////////////////////////////////////////
// Filmic (Hable style)
//////////////////////////////////////////////////////////

public float3 filmic(float3 x)
{
    float3 X = max(float3(0.0), x - 0.004);

    float3 result =
        (X * (6.2 * X + 0.5)) /
        (X * (6.2 * X + 1.7) + 0.06);

    return pow(result, float3(2.2));
}

public float filmic(float x)
{
    float X = max(0.0, x - 0.004);

    float result =
        (X * (6.2 * X + 0.5)) /
        (X * (6.2 * X + 1.7) + 0.06);

    return pow(result, 2.2);
}

//////////////////////////////////////////////////////////
// Unreal
//////////////////////////////////////////////////////////

public float3 unreal(float3 x)
{
    return x / (x + 0.155) * 1.019;
}

public float unreal(float x)
{
    return x / (x + 0.155) * 1.019;
}

//////////////////////////////////////////////////////////
// Uncharted2
//////////////////////////////////////////////////////////

internal float3 uncharted2Tonemap(float3 x)
{
    float A = 0.15;
    float B = 0.50;
    float C = 0.10;
    float D = 0.20;
    float E = 0.02;
    float F = 0.30;

    return ((x * (A * x + C * B) + D * E) /
           (x * (A * x + B) + D * F)) - E / F;
}

public float3 uncharted2(float3 color)
{
    const float W = 11.2;
    const float exposureBias = 2.0;

    float3 curr = uncharted2Tonemap(exposureBias * color);
    float3 whiteScale = 1.0 / uncharted2Tonemap(float3(W));

    return curr * whiteScale;
}

//////////////////////////////////////////////////////////
// Neutral (Khronos PBR)
//////////////////////////////////////////////////////////

public float3 neutral(float3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;

    color -= offset;

    float peak = max(color.r, max(color.g, color.b));

    if(peak < startCompression)
        return color;

    float d = 1.0 - startCompression;

    float newPeak =
        1.0 - d * d /
        (peak + d - startCompression);

    color *= newPeak / peak;

    float g =
        1.0 - 1.0 /
        (desaturation * (peak - newPeak) + 1.0);

    return lerp(color, float3(newPeak), g);
}















