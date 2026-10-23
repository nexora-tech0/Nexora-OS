/*
 * Nexora OS - user/include/hzcore.h
 * The small C++ foundation user programs use instead of a standard library (there is none):
 * Move/Forward, placement new, Vector, String (UTF-8), UniquePtr, Function and a few helpers.
 * Memory comes from the hzrt heap. Nothing throws: running out of memory ends the process with a
 * message, as operator new does.
 */
#ifndef HZCORE_H
#define HZCORE_H

#include "hzsys.h"

inline void *operator new(size_t, void *Where) noexcept
{
    return Where;
}

inline void operator delete(void *, void *) noexcept
{
}

namespace hz {

/* ---- Type helpers ------------------------------------------------------------------------------ */

template <class T> struct RemoveReference { using Type = T; };
template <class T> struct RemoveReference<T &> { using Type = T; };
template <class T> struct RemoveReference<T &&> { using Type = T; };
template <class T> struct RemoveCv { using Type = T; };
template <class T> struct RemoveCv<const T> { using Type = T; };
template <class T> struct RemoveCv<volatile T> { using Type = T; };
template <class T> struct RemoveCv<const volatile T> { using Type = T; };
template <class T> using RemoveCvRef = typename RemoveCv<typename RemoveReference<T>::Type>::Type;
/* Functions decay to function pointers (for Function's stored callable). */
template <class T> struct DecayFunction { using Type = T; };
template <class R, class... A> struct DecayFunction<R(A...)> { using Type = R (*)(A...); };
template <class T> using Decay = typename DecayFunction<RemoveCvRef<T>>::Type;
template <class A, class B> inline constexpr bool IsSame = false;
template <class A> inline constexpr bool IsSame<A, A> = true;

template <class T>
constexpr typename RemoveReference<T>::Type &&Move(T &&Value) noexcept
{
    return static_cast<typename RemoveReference<T>::Type &&>(Value);
}

template <class T>
constexpr T &&Forward(typename RemoveReference<T>::Type &Value) noexcept
{
    return static_cast<T &&>(Value);
}

template <class T>
constexpr T &&Forward(typename RemoveReference<T>::Type &&Value) noexcept
{
    return static_cast<T &&>(Value);
}

template <class T>
void Swap(T &A, T &B)
{
    T Temp = Move(A);
    A = Move(B);
    B = Move(Temp);
}

template <class T>
constexpr T Min(T A, T B)
{
    return B < A ? B : A;
}

template <class T>
constexpr T Max(T A, T B)
{
    return A < B ? B : A;
}

template <class T>
constexpr T Clamp(T Value, T Low, T High)
{
    return Value < Low ? Low : (High < Value ? High : Value);
}

constexpr size_t NotFound = (size_t)-1;

[[noreturn]] inline void OutOfMemory()
{
    HzPrint("hzcore: out of memory");
    HzExitProcess(STATUS_NO_MEMORY);
}

/* ---- Vector ------------------------------------------------------------------------------------ */

template <class T>
class Vector {
public:
    Vector() = default;

    Vector(const Vector &Other)
    {
        Reserve(Other.Size_);
        for (size_t i = 0; i < Other.Size_; i++)
            new (&Data_[i]) T(Other.Data_[i]);
        Size_ = Other.Size_;
    }

    Vector(Vector &&Other) noexcept : Data_(Other.Data_), Size_(Other.Size_), Capacity_(Other.Capacity_)
    {
        Other.Data_ = nullptr;
        Other.Size_ = Other.Capacity_ = 0;
    }

    ~Vector()
    {
        Clear();
        free(Data_);
    }

    Vector &operator=(const Vector &Other)
    {
        if (this != &Other) {
            Vector Copy(Other);
            *this = Move(Copy);
        }
        return *this;
    }

    Vector &operator=(Vector &&Other) noexcept
    {
        if (this != &Other) {
            Clear();
            free(Data_);
            Data_ = Other.Data_;
            Size_ = Other.Size_;
            Capacity_ = Other.Capacity_;
            Other.Data_ = nullptr;
            Other.Size_ = Other.Capacity_ = 0;
        }
        return *this;
    }

    size_t Size() const { return Size_; }
    bool Empty() const { return Size_ == 0; }
    T *Data() { return Data_; }
    const T *Data() const { return Data_; }
    T &operator[](size_t Index) { return Data_[Index]; }
    const T &operator[](size_t Index) const { return Data_[Index]; }
    T &Front() { return Data_[0]; }
    T &Back() { return Data_[Size_ - 1]; }
    const T &Back() const { return Data_[Size_ - 1]; }
    T *begin() { return Data_; }
    T *end() { return Data_ + Size_; }
    const T *begin() const { return Data_; }
    const T *end() const { return Data_ + Size_; }

    void Reserve(size_t Capacity)
    {
        if (Capacity <= Capacity_)
            return;
        T *New = (T *)malloc(Capacity * sizeof(T));
        if (!New)
            OutOfMemory();
        for (size_t i = 0; i < Size_; i++) {
            new (&New[i]) T(Move(Data_[i]));
            Data_[i].~T();
        }
        free(Data_);
        Data_ = New;
        Capacity_ = Capacity;
    }

    void Push(const T &Value)
    {
        if (Size_ == Capacity_) {
            T Copy(Value); /* Value may live in this vector */
            Grow();
            new (&Data_[Size_]) T(Move(Copy));
        } else {
            new (&Data_[Size_]) T(Value);
        }
        Size_++;
    }

    void Push(T &&Value)
    {
        if (Size_ == Capacity_) {
            T Copy(Move(Value));
            Grow();
            new (&Data_[Size_]) T(Move(Copy));
        } else {
            new (&Data_[Size_]) T(Move(Value));
        }
        Size_++;
    }

    template <class... A>
    T &Emplace(A &&...Args)
    {
        if (Size_ == Capacity_)
            Grow();
        new (&Data_[Size_]) T(Forward<A>(Args)...);
        return Data_[Size_++];
    }

    void Pop()
    {
        Data_[--Size_].~T();
    }

    void Insert(size_t At, T Value)
    {
        if (At >= Size_) {
            Push(Move(Value));
            return;
        }
        if (Size_ == Capacity_)
            Grow();
        new (&Data_[Size_]) T(Move(Data_[Size_ - 1]));
        for (size_t i = Size_ - 1; i > At; i--)
            Data_[i] = Move(Data_[i - 1]);
        Data_[At] = Move(Value);
        Size_++;
    }

    void Erase(size_t At)
    {
        for (size_t i = At; i + 1 < Size_; i++)
            Data_[i] = Move(Data_[i + 1]);
        Pop();
    }

    void Clear()
    {
        while (Size_)
            Pop();
    }

    void Resize(size_t Count)
    {
        Reserve(Count);
        while (Size_ < Count)
            new (&Data_[Size_++]) T();
        while (Size_ > Count)
            Pop();
    }

    size_t Find(const T &Value) const
    {
        for (size_t i = 0; i < Size_; i++)
            if (Data_[i] == Value)
                return i;
        return NotFound;
    }

    bool Contains(const T &Value) const { return Find(Value) != NotFound; }

    bool Remove(const T &Value)
    {
        size_t Index = Find(Value);
        if (Index == NotFound)
            return false;
        Erase(Index);
        return true;
    }

private:
    void Grow() { Reserve(Capacity_ ? Capacity_ * 2 : 4); }

    T *Data_ = nullptr;
    size_t Size_ = 0;
    size_t Capacity_ = 0;
};

/* ---- String (UTF-8 bytes, always NUL-terminated) ------------------------------------------------ */

class String {
public:
    String() = default;
    String(const char *Text) { Append(Text); }
    String(const char *Text, size_t Length) { Append(Text, Length); }
    String(const String &Other) { Append(Other.CStr(), Other.Length_); }
    String(String &&Other) noexcept : Data_(Other.Data_), Length_(Other.Length_), Capacity_(Other.Capacity_)
    {
        Other.Data_ = nullptr;
        Other.Length_ = Other.Capacity_ = 0;
    }
    ~String() { free(Data_); }

    String &operator=(const String &Other)
    {
        if (this != &Other) {
            Clear();
            Append(Other.CStr(), Other.Length_);
        }
        return *this;
    }

    String &operator=(String &&Other) noexcept
    {
        if (this != &Other) {
            free(Data_);
            Data_ = Other.Data_;
            Length_ = Other.Length_;
            Capacity_ = Other.Capacity_;
            Other.Data_ = nullptr;
            Other.Length_ = Other.Capacity_ = 0;
        }
        return *this;
    }

    String &operator=(const char *Text)
    {
        String Copy(Text); /* Text may point into this string */
        return *this = Move(Copy);
    }

    const char *CStr() const { return Data_ ? Data_ : ""; }
    size_t Length() const { return Length_; }
    bool Empty() const { return Length_ == 0; }
    char operator[](size_t Index) const { return Data_[Index]; }

    void Append(const char *Text, size_t Length);
    void Append(const char *Text) { Append(Text, Text ? strlen(Text) : 0); }
    void Append(const String &Other) { Append(Other.CStr(), Other.Length_); }
    void Append(char C) { Append(&C, 1); }
    void AppendCodePoint(uint32_t CodePoint);
    void Insert(size_t At, const char *Text, size_t Length);
    void Erase(size_t At, size_t Count);
    void Clear()
    {
        Length_ = 0;
        if (Data_)
            Data_[0] = '\0';
    }

    String Substring(size_t At, size_t Count = NotFound) const;
    size_t Find(const char *Needle, size_t From = 0) const;
    bool StartsWith(const char *Prefix) const;
    bool ContainsIgnoreCase(const char *Needle) const;

    bool operator==(const String &Other) const { return Length_ == Other.Length_ && memcmp(CStr(), Other.CStr(), Length_) == 0; }
    bool operator==(const char *Other) const { return strcmp(CStr(), Other ? Other : "") == 0; }
    bool operator!=(const String &Other) const { return !(*this == Other); }
    bool operator!=(const char *Other) const { return !(*this == Other); }
    String &operator+=(const char *Text)
    {
        Append(Text);
        return *this;
    }
    String &operator+=(const String &Other)
    {
        Append(Other);
        return *this;
    }

    static String Format(const char *Format, ...) HZ_PRINTF(1, 2);

    /* UTF-8 navigation over byte offsets (offsets always sit on code point boundaries). */
    size_t NextChar(size_t At) const;
    size_t PreviousChar(size_t At) const;
    uint32_t CodePointAt(size_t At) const;

private:
    void Reserve(size_t Capacity);

    char *Data_ = nullptr;
    size_t Length_ = 0;
    size_t Capacity_ = 0; /* bytes available for text (the NUL comes on top) */
};

/* ---- UniquePtr --------------------------------------------------------------------------------- */

template <class T>
class UniquePtr {
public:
    UniquePtr() = default;
    UniquePtr(decltype(nullptr)) {}
    explicit UniquePtr(T *Pointer) : Pointer_(Pointer) {}
    UniquePtr(UniquePtr &&Other) noexcept : Pointer_(Other.Release()) {}
    template <class U>
    UniquePtr(UniquePtr<U> &&Other) noexcept : Pointer_(Other.Release())
    {
    }
    UniquePtr(const UniquePtr &) = delete;
    UniquePtr &operator=(const UniquePtr &) = delete;
    UniquePtr &operator=(UniquePtr &&Other) noexcept
    {
        Reset(Other.Release());
        return *this;
    }
    ~UniquePtr() { delete Pointer_; }

    T *Get() const { return Pointer_; }
    T *operator->() const { return Pointer_; }
    T &operator*() const { return *Pointer_; }
    explicit operator bool() const { return Pointer_ != nullptr; }
    T *Release()
    {
        T *Pointer = Pointer_;
        Pointer_ = nullptr;
        return Pointer;
    }
    void Reset(T *Pointer = nullptr)
    {
        T *Old = Pointer_;
        Pointer_ = Pointer;
        delete Old;
    }

private:
    T *Pointer_ = nullptr;
};

template <class T, class... A>
UniquePtr<T> MakeUnique(A &&...Args)
{
    return UniquePtr<T>(new T(Forward<A>(Args)...));
}

/* ---- Function ------------------------------------------------------------------------------------ */

template <class Signature>
class Function;

template <class R, class... A>
class Function<R(A...)> {
    struct Callable {
        virtual ~Callable() = default;
        virtual R Invoke(A... Args) = 0;
        virtual Callable *Clone() const = 0;
    };

    template <class F>
    struct Holder final : Callable {
        F Target;
        explicit Holder(const F &T) : Target(T) {}
        explicit Holder(F &&T) : Target(Move(T)) {}
        R Invoke(A... Args) override { return Target(Forward<A>(Args)...); }
        Callable *Clone() const override { return new Holder(Target); }
    };

public:
    Function() = default;
    Function(decltype(nullptr)) {}

    template <class F>
        requires(!IsSame<RemoveCvRef<F>, Function>)
    Function(F &&Target) : Callable_(new Holder<Decay<F>>(Forward<F>(Target)))
    {
    }

    Function(const Function &Other) : Callable_(Other.Callable_ ? Other.Callable_->Clone() : nullptr) {}
    Function(Function &&Other) noexcept : Callable_(Other.Callable_) { Other.Callable_ = nullptr; }
    Function &operator=(const Function &Other)
    {
        if (this != &Other) {
            delete Callable_;
            Callable_ = Other.Callable_ ? Other.Callable_->Clone() : nullptr;
        }
        return *this;
    }
    Function &operator=(Function &&Other) noexcept
    {
        if (this != &Other) {
            delete Callable_;
            Callable_ = Other.Callable_;
            Other.Callable_ = nullptr;
        }
        return *this;
    }
    ~Function() { delete Callable_; }

    explicit operator bool() const { return Callable_ != nullptr; }
    R operator()(A... Args) const { return Callable_->Invoke(Forward<A>(Args)...); }

private:
    Callable *Callable_ = nullptr;
};

}  // namespace hz

#endif /* HZCORE_H */
