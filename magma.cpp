#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <iomanip>
#include <string>
#include <chrono>  

using namespace std;
using namespace std::chrono;  

// S-блоки
const uint8_t S_BOXES[8][16] = {
    {12, 4, 6, 2, 10, 5, 11, 9, 14, 8, 13, 7, 0, 3, 15, 1},  // π0'
    {6, 8, 2, 3, 9, 10, 5, 12, 1, 14, 4, 7, 11, 13, 0, 15},   // π1'
    {11, 3, 5, 8, 2, 15, 10, 13, 14, 1, 7, 4, 12, 9, 6, 0},   // π2'
    {12, 8, 2, 1, 13, 4, 15, 6, 7, 0, 10, 5, 3, 14, 9, 11},   // π3'
    {7, 15, 5, 10, 8, 1, 6, 13, 0, 9, 3, 14, 11, 4, 2, 12},   // π4'
    {5, 13, 15, 6, 9, 2, 12, 10, 11, 7, 8, 1, 4, 3, 14, 0},   // π5'
    {8, 14, 2, 5, 6, 9, 1, 12, 15, 4, 11, 0, 13, 10, 3, 7},   // π6'
    {1, 7, 14, 13, 0, 5, 8, 3, 4, 15, 10, 6, 9, 12, 11, 2}     // π7'
};

// Функция подстановки t
uint32_t t(uint32_t a) {
    uint32_t result = 0;
    
    for (int i = 0; i < 8; i++) {
        uint8_t nibble = (a >> (4 * i)) & 0xF;
        uint8_t substituted = S_BOXES[i][nibble];
        result |= (uint32_t)substituted << (4 * i);
    }
    
    return result;
}

// Функция g[k]
uint32_t g(uint32_t a, uint32_t k) {
    // Сложение с ключом по модулю 2^32
    uint32_t sum = a + k;
    
    // Применение подстановки t
    uint32_t substituted = t(sum);
    
    // Циклический сдвиг на 11 бит влево
    uint32_t rotated = (substituted << 11) | (substituted >> (32 - 11));
    
    return rotated;
}

// Один раунд шифрования
void round(uint32_t& left, uint32_t& right, uint32_t round_key) {
    uint32_t temp = right;
    right = left ^ g(right, round_key);
    left = temp;
}

// Генерация раундовых ключей из 256-битного ключа
void generate_round_keys(const uint32_t* key_256, uint32_t* round_keys) {
    // Первые 24 раунда используют ключи k1..k8 в прямом порядке
    for (int i = 0; i < 24; i++) {
        round_keys[i] = key_256[i % 8];
    }
    
    // Последние 8 раундов используют ключи k8..k1 в обратном порядке
    for (int i = 0; i < 8; i++) {
        round_keys[24 + i] = key_256[7 - i];
    }
}

// Расширение 56-битного ключа до 256 бит
void expand_key(const uint8_t* key_56, uint32_t* key_256) {
    // Повторяем ключ 4 раза (4*56 = 224 бита)
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 7; j++) {
            key_256[i * 7 + j] = key_56[j];
        }
    }
    
    // Добавляем старшие 4 байта 
    for (int i = 0; i < 4; i++) {
        key_256[28 + i] = key_56[i];
    }
}

// Зашифрование одного 64-битного блока
void encrypt_block(uint32_t& left, uint32_t& right, const uint32_t* round_keys) {
    // 32 раунда шифрования
    for (int i = 0; i < 32; i++) {
        round(left, right, round_keys[i]);
    }
    
    // В последнем раунде не меняем местами левую и правую части
    uint32_t temp = left;
    left = right;
    right = temp;
}

// Расшифрование одного 64-битного блока
void decrypt_block(uint32_t& left, uint32_t& right, const uint32_t* round_keys) {
    // Для дешифрования используем ключи в обратном порядке
    for (int i = 31; i >= 0; i--) {
        round(left, right, round_keys[i]);
    }
    
    // В последнем раунде не меняем местами левую и правую части
    uint32_t temp = left;
    left = right;
    right = temp;
}

// Чтение ключа из файла
bool read_key(const string& filename, uint8_t* key_56) {
    ifstream key_file(filename, ios::binary);
    if (!key_file) {
        cerr << "Ошибка открытия файла ключа" << endl;
        return false;
    }
    
    key_file.read(reinterpret_cast<char*>(key_56), 7);
    if (key_file.gcount() != 7) {
        cerr << "Неверный размер ключа (должен быть 56 бит)" << endl;
        return false;
    }
    
    return true;
}

// Дополнение блока 
void pad_block(vector<uint8_t>& data) {
    // Добавляем бит '1'
    data.push_back(0x80);
    
    // Добавляем биты '0' до размера кратного 8 байтам
    while (data.size() % 8 != 0) {
        data.push_back(0x00);
    }
}

// Удаление дополнения после расшифрования
void unpad_block(vector<uint8_t>& data) {
    if (data.empty()) return;

    // Находим последний бит '1'
    size_t pos = data.size() - 1;
    while (pos > 0 && data[pos] == 0x00) {
        pos--;
    }
    
    if (data[pos] == 0x80) {
        data.resize(pos);
    }
}

// Проверка на необходимость смены ключа
bool check_key_usage(size_t bytes_processed, bool is_encryption) {
    if (bytes_processed > 20 * 1024) {
        cerr << "Ошибка: превышен максимальный объем данных для одного ключа (20 КБ)" << endl;
        return false;
    }
    else if (bytes_processed > 10 * 1024) {
        cerr << "Предупреждение: рекомендуется сменить ключ (обработано более 10 КБ)" << endl;
    }
    return true;
}

int main(int argc, char* argv[]) {
    auto start = high_resolution_clock::now();
    if (argc != 2 || (string(argv[1]) != "-e" && string(argv[1]) != "-d")) {
        cerr << "Использование: " << argv[0] << " -e (зашифрование) или -d (расшифрование)" << endl;
        return 1;
    }
    
    bool encrypt_mode = (string(argv[1]) == "-e");
    // 1. Загрузка ключа
    uint8_t key_56[7]; // 56 бит = 7 байт
    if (!read_key("key.key", key_56)) {
        return 1;
    }
    // 2. Расширение ключа до 256 бит
    uint32_t key_256[8]; // 256 бит = 8 * 32 бита
    expand_key(key_56, key_256);
    // 3. Генерация раундовых ключей
    uint32_t round_keys[32];
    generate_round_keys(key_256, round_keys);
    // 4. Чтение входного файла
    string input_filename = encrypt_mode ? "input.txt" : "output.enc";
    string output_filename = encrypt_mode ? "output.enc" : "output.txt";
    
    ifstream input_file(input_filename, ios::binary);
    if (!input_file) {
        cerr << "Ошибка открытия входного файла " << input_filename << endl;
        return 1;
    }
    
    // 5. Чтение данных
    vector<uint8_t> input_data((istreambuf_iterator<char>(input_file)), 
                             istreambuf_iterator<char>());
    input_file.close();
    
    // 6. Проверка объема данных
    if (!check_key_usage(input_data.size(), encrypt_mode)) {
        return 1;
    }
    
    // 7. Дополнение только при зашифровании
    if (encrypt_mode && input_data.size() % 8 != 0) {
        pad_block(input_data);
    }
    
    // 8. Обработка данных
    vector<uint8_t> output_data;
    for (size_t i = 0; i < input_data.size(); i += 8) {
        // Если осталось меньше 8 байт, дополняем нулями (для расшифрования)
        if (i + 8 > input_data.size()) {
            if (!encrypt_mode) {
                // Для расшифрования просто копируем оставшиеся байты
                for (size_t j = i; j < input_data.size(); j++) {
                    output_data.push_back(input_data[j]);
                }
                break;
            } else {
                cerr << "Ошибка: размер данных не кратен 64 битам" << endl;
                return 1;
            }
        }
        
        // Получаем 64-битный блок
        uint32_t left = (input_data[i] << 24) | (input_data[i+1] << 16) | 
                        (input_data[i+2] << 8) | input_data[i+3];
        uint32_t right = (input_data[i+4] << 24) | (input_data[i+5] << 16) | 
                         (input_data[i+6] << 8) | input_data[i+7];
        
        // Зашифруем или расшифровываем блок
        if (encrypt_mode) {
            encrypt_block(left, right, round_keys);
        } else {
            decrypt_block(left, right, round_keys);
        }
        
        // Сохраняем результат
        output_data.push_back((left >> 24) & 0xFF);
        output_data.push_back((left >> 16) & 0xFF);
        output_data.push_back((left >> 8) & 0xFF);
        output_data.push_back(left & 0xFF);
        output_data.push_back((right >> 24) & 0xFF);
        output_data.push_back((right >> 16) & 0xFF);
        output_data.push_back((right >> 8) & 0xFF);
        output_data.push_back(right & 0xFF);
    }
    
    // 9. Удаление дополнения после расшифрования
    if (!encrypt_mode) {
        unpad_block(output_data);
    }
    
    // 10. Сохранение результата
    ofstream output_file(output_filename, ios::binary);
    if (!output_file) {
        cerr << "Ошибка создания выходного файла " << output_filename << endl;
        return 1;
    }
    output_file.write(reinterpret_cast<const char*>(output_data.data()), output_data.size());
    output_file.close();
    
    // Конец измерения времени
    auto stop = high_resolution_clock::now();
auto duration = duration_cast<milliseconds>(stop - start);

cout << (encrypt_mode ? "Зашифрование" : "Расшифрование") << " завершено успешно" << endl;
cout << "Затраченное время: " << duration.count() / 1000.0 << " секунд" << endl;
    
    // Для тестирования: сравнение исходных и расшифрованных данных
    if (!encrypt_mode) {
        ifstream original_file("input.txt", ios::binary);
        if (original_file) {
            vector<uint8_t> original_data((istreambuf_iterator<char>(original_file)), 
                                        istreambuf_iterator<char>());
            original_file.close();
            
            if (original_data == output_data) {
                cout << "Расшифрованные данные совпадают с исходными" << endl;
            } else {
                cout << "Внимание: расшифрованные данные НЕ совпадают с исходными" << endl;
            }
        }
    }
    
    return 0;
}
